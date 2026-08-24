// SPDX-License-Identifier: GPL-2.0

#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/videodev2.h>

#include <asm/unaligned.h>

#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "../include/uapi/linux/zzh_mlx90640_meta.h"

#define ZZH_MLX90640_DRIVER_NAME          "zzh_mlx90640"
#define ZZH_MLX90640_I2C_ADDRESS          0x33
#define ZZH_MLX90640_PIXEL_START          0x0400
#define ZZH_MLX90640_AUX_START            0x0700
#define ZZH_MLX90640_STATUS               0x8000
#define ZZH_MLX90640_CONTROL              0x800d
#define ZZH_MLX90640_EEPROM_START         0x2400
#define ZZH_MLX90640_EEPROM_WORDS         832
#define ZZH_MLX90640_EEPROM_BYTES         1664
#define ZZH_MLX90640_ID_OFFSET            7
#define ZZH_MLX90640_CHUNK_WORDS          32
#define ZZH_MLX90640_I2C_RETRIES          3
#define ZZH_MLX90640_STATUS_READY         BIT(3)
#define ZZH_MLX90640_STATUS_SUBPAGE       BIT(0)
#define ZZH_MLX90640_STATUS_CLEAR         0x0030
#define ZZH_MLX90640_CONTROL_REFRESH_MASK GENMASK(9, 7)
#define ZZH_MLX90640_CONTROL_CHESS        BIT(12)
#define ZZH_MLX90640_WARMUP_SUBPAGES      2
#define ZZH_MLX90640_READY_TIMEOUT_NS     (2ULL * NSEC_PER_SEC)

struct zzh_mlx90640_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct zzh_mlx90640_sample {
	u64 ready_ns;
	u64 read_done_ns;
	u16 status;
	u16 control;
	u16 subpage_id;
	u16 pixels[ZZH_MLX90640_META_PIXEL_WORDS];
	u16 auxiliary[ZZH_MLX90640_META_AUX_WORDS];
};

struct zzh_mlx90640 {
	struct i2c_client *client;
	struct mutex lock; /* Serializes V4L2 ioctls. */
	struct mutex queue_lock; /* Serializes VB2 queue operations. */
	spinlock_t slock; /* Protects active_buffers. */
	struct list_head active_buffers;

	struct v4l2_device v4l2_dev;
	struct video_device video_dev;
	struct vb2_queue queue;
	struct task_struct *capture_thread;

	u32 pair_sequence;
	u32 subpage_rate_hz;
	u32 dropped_no_buffer;
	u32 duplicate_subpage;
	u16 original_control;
	u16 active_control;
	bool control_modified;
	struct zzh_mlx90640_sample pair_samples[2];

	u8 eeprom_be[ZZH_MLX90640_EEPROM_BYTES];
	char nvmem_name[40];
	struct nvmem_config nvmem_config;
};

static int zzh_mlx90640_transfer(struct zzh_mlx90640 *mlx,
				 struct i2c_msg *messages, int count)
{
	int attempt;
	int ret = -EIO;

	for (attempt = 0; attempt < ZZH_MLX90640_I2C_RETRIES; attempt++) {
		ret = i2c_transfer(mlx->client->adapter, messages, count);
		if (ret == count)
			return 0;
		if (ret >= 0)
			ret = -EIO;
		if (ret == -EINTR)
			continue;
		usleep_range(2000, 3000);
	}

	return ret;
}

static int zzh_mlx90640_read_words(struct zzh_mlx90640 *mlx, u16 start,
				   u16 *values, size_t count)
{
	u8 address[2];
	u8 raw[ZZH_MLX90640_CHUNK_WORDS * 2];
	size_t offset = 0;

	while (offset < count) {
		struct i2c_msg messages[2];
		size_t words = min_t(size_t, count - offset,
				     ZZH_MLX90640_CHUNK_WORDS);
		u16 reg = start + offset;
		size_t i;
		int ret;

		address[0] = reg >> 8;
		address[1] = reg & 0xff;
		messages[0].addr = mlx->client->addr;
		messages[0].flags = 0;
		messages[0].len = sizeof(address);
		messages[0].buf = address;
		messages[1].addr = mlx->client->addr;
		messages[1].flags = I2C_M_RD;
		messages[1].len = words * 2;
		messages[1].buf = raw;

		ret = zzh_mlx90640_transfer(mlx, messages, ARRAY_SIZE(messages));
		if (ret) {
			dev_err_ratelimited(&mlx->client->dev,
					    "read 0x%04x..0x%04x failed: %d\n", reg,
				(unsigned int)(reg + words - 1), ret);
			return ret;
		}

		for (i = 0; i < words; i++)
			values[offset + i] = get_unaligned_be16(&raw[i * 2]);
		offset += words;
	}

	return 0;
}

static int zzh_mlx90640_write_word(struct zzh_mlx90640 *mlx, u16 reg,
				   u16 value)
{
	u8 raw[4] = {
		reg >> 8,
		reg & 0xff,
		value >> 8,
		value & 0xff,
	};
	struct i2c_msg message = {
		.addr = mlx->client->addr,
		.flags = 0,
		.len = sizeof(raw),
		.buf = raw,
	};
	int ret;

	ret = zzh_mlx90640_transfer(mlx, &message, 1);
	if (ret)
		dev_err_ratelimited(&mlx->client->dev,
				    "write 0x%04x=0x%04x failed: %d\n", reg, value, ret);

	return ret;
}

static int zzh_mlx90640_write_control(struct zzh_mlx90640 *mlx, u16 value)
{
	u16 readback;
	int ret;

	ret = zzh_mlx90640_write_word(mlx, ZZH_MLX90640_CONTROL, value);
	if (ret)
		return ret;
	ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_CONTROL,
				      &readback, 1);
	if (ret)
		return ret;
	if (readback != value) {
		dev_err(&mlx->client->dev,
			"control readback mismatch: wrote 0x%04x read 0x%04x\n",
			value, readback);
		return -EIO;
	}

	return 0;
}

static int zzh_mlx90640_validate_pixels(const u16 *pixels, u16 subpage)
{
	unsigned int line;

	for (line = 0; line < 24; line++)
		if (pixels[line * 32] == 0x7fff && line % 2 == subpage)
			return -EILSEQ;

	return 0;
}

static int zzh_mlx90640_validate_aux(const u16 *aux)
{
	unsigned int i;

	if (aux[0] == 0x7fff)
		return -EILSEQ;
	for (i = 8; i < 19; i++)
		if (aux[i] == 0x7fff)
			return -EILSEQ;
	for (i = 20; i < 23; i++)
		if (aux[i] == 0x7fff)
			return -EILSEQ;
	for (i = 24; i < 33; i++)
		if (aux[i] == 0x7fff)
			return -EILSEQ;
	for (i = 40; i < 51; i++)
		if (aux[i] == 0x7fff)
			return -EILSEQ;
	for (i = 52; i < 55; i++)
		if (aux[i] == 0x7fff)
			return -EILSEQ;
	for (i = 56; i < 64; i++)
		if (aux[i] == 0x7fff)
			return -EILSEQ;

	return 0;
}

static int zzh_mlx90640_wait_ready(struct zzh_mlx90640 *mlx, u16 *status,
				   u64 *ready_ns, bool stoppable)
{
	u64 start_ns = ktime_get_ns();
	int ret;

	for (;;) {
		if (stoppable && kthread_should_stop())
			return -EINTR;
		ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_STATUS,
					      status, 1);
		if (ret)
			return ret;
		if (*status & ZZH_MLX90640_STATUS_READY) {
			*ready_ns = ktime_get_ns();
			return 0;
		}
		if (ktime_get_ns() - start_ns > ZZH_MLX90640_READY_TIMEOUT_NS)
			return -ETIMEDOUT;
		usleep_range(1000, 1500);
	}
}

static int zzh_mlx90640_capture_subpage(struct zzh_mlx90640 *mlx,
					struct zzh_mlx90640_sample *sample,
					bool validate, bool stoppable)
{
	int ret;

	ret = zzh_mlx90640_wait_ready(mlx, &sample->status,
				      &sample->ready_ns, stoppable);
	if (ret)
		return ret;
	sample->subpage_id = sample->status & ZZH_MLX90640_STATUS_SUBPAGE;

	ret = zzh_mlx90640_write_word(mlx, ZZH_MLX90640_STATUS,
				      ZZH_MLX90640_STATUS_CLEAR);
	if (ret)
		return ret;
	ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_PIXEL_START,
				      sample->pixels,
					ZZH_MLX90640_META_PIXEL_WORDS);
	if (ret)
		return ret;
	ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_AUX_START,
				      sample->auxiliary,
					ZZH_MLX90640_META_AUX_WORDS);
	if (ret)
		return ret;
	ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_CONTROL,
				      &sample->control, 1);
	if (ret)
		return ret;
	sample->read_done_ns = ktime_get_ns();

	if (sample->control != mlx->active_control)
		return -EIO;
	if (!validate)
		return 0;
	ret = zzh_mlx90640_validate_pixels(sample->pixels,
					   sample->subpage_id);
	if (ret)
		return ret;

	return zzh_mlx90640_validate_aux(sample->auxiliary);
}

static int zzh_mlx90640_rate_code(u32 rate_hz)
{
	if (rate_hz == 8)
		return 4;
	if (rate_hz == 16)
		return 5;

	return -EINVAL;
}

static int zzh_mlx90640_restore_control(struct zzh_mlx90640 *mlx)
{
	int ret = 0;

	if (mlx->control_modified) {
		ret = zzh_mlx90640_write_control(mlx, mlx->original_control);
		if (ret)
			dev_err(&mlx->client->dev,
				"failed to restore control 0x%04x: %d\n",
				mlx->original_control, ret);
		else
			dev_info(&mlx->client->dev,
				 "restored control 0x%04x\n",
				 mlx->original_control);
	}
	mlx->control_modified = false;

	return ret;
}

static int zzh_mlx90640_prepare_stream(struct zzh_mlx90640 *mlx)
{
	u16 desired;
	int rate_code;
	int i;
	int ret;

	ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_CONTROL,
				      &mlx->original_control, 1);
	if (ret)
		return ret;
	rate_code = zzh_mlx90640_rate_code(mlx->subpage_rate_hz);
	if (rate_code < 0)
		return rate_code;

	desired = mlx->original_control & ~ZZH_MLX90640_CONTROL_REFRESH_MASK;
	desired |= rate_code << 7;
	desired |= ZZH_MLX90640_CONTROL_CHESS;
	if (desired != mlx->original_control) {
		ret = zzh_mlx90640_write_control(mlx, desired);
		if (ret)
			return ret;
		mlx->control_modified = true;
	}
	mlx->active_control = desired;

	ret = zzh_mlx90640_write_word(mlx, ZZH_MLX90640_STATUS,
				      ZZH_MLX90640_STATUS_CLEAR);
	if (ret)
		goto restore;
	for (i = 0; i < ZZH_MLX90640_WARMUP_SUBPAGES; i++) {
		ret = zzh_mlx90640_capture_subpage(mlx,
						   &mlx->pair_samples[0], false, false);
		if (ret)
			goto restore;
	}

	return 0;

restore:
	zzh_mlx90640_restore_control(mlx);
	return ret;
}

static void zzh_mlx90640_copy_subpage(struct zzh_mlx90640_subpage_v1 *dst,
				      const struct zzh_mlx90640_sample *src)
{
	unsigned int i;

	dst->ready_timestamp_ns = cpu_to_le64(src->ready_ns);
	dst->read_done_timestamp_ns = cpu_to_le64(src->read_done_ns);
	dst->status = cpu_to_le16(src->status);
	dst->control = cpu_to_le16(src->control);
	dst->subpage_id = cpu_to_le16(src->subpage_id);
	for (i = 0; i < ZZH_MLX90640_META_PIXEL_WORDS; i++)
		dst->pixels[i] = cpu_to_le16(src->pixels[i]);
	for (i = 0; i < ZZH_MLX90640_META_AUX_WORDS; i++)
		dst->auxiliary[i] = cpu_to_le16(src->auxiliary[i]);
}

static struct zzh_mlx90640_buffer *
zzh_mlx90640_pop_buffer(struct zzh_mlx90640 *mlx)
{
	struct zzh_mlx90640_buffer *buffer = NULL;
	unsigned long flags;

	spin_lock_irqsave(&mlx->slock, flags);
	if (!list_empty(&mlx->active_buffers)) {
		buffer = list_first_entry(&mlx->active_buffers,
					  struct zzh_mlx90640_buffer, list);
		list_del(&buffer->list);
	}
	spin_unlock_irqrestore(&mlx->slock, flags);

	return buffer;
}

static void zzh_mlx90640_complete_pair(struct zzh_mlx90640 *mlx,
				       const struct zzh_mlx90640_sample *first,
				       const struct zzh_mlx90640_sample *second)
{
	struct zzh_mlx90640_buffer *buffer;
	struct zzh_mlx90640_meta_v1 *meta;
	struct vb2_buffer *vb;
	u32 sequence;

	buffer = zzh_mlx90640_pop_buffer(mlx);
	if (!buffer) {
		mlx->dropped_no_buffer++;
		return;
	}

	vb = &buffer->vb.vb2_buf;
	meta = vb2_plane_vaddr(vb, 0);
	memset(meta, 0, sizeof(*meta));
	sequence = mlx->pair_sequence++;
	meta->magic = cpu_to_le32(ZZH_MLX90640_META_MAGIC);
	meta->version = cpu_to_le16(ZZH_MLX90640_META_VERSION);
	meta->header_size = cpu_to_le16(ZZH_MLX90640_META_V1_HEADER_SIZE);
	meta->buffer_size = cpu_to_le32(sizeof(*meta));
	meta->pair_sequence = cpu_to_le32(sequence);
	meta->flags = cpu_to_le32(ZZH_MLX90640_META_FLAG_CHESS |
					ZZH_MLX90640_META_FLAG_CONSECUTIVE);
	zzh_mlx90640_copy_subpage(&meta->subpage[0], first);
	zzh_mlx90640_copy_subpage(&meta->subpage[1], second);

	vb->timestamp = second->ready_ns;
	buffer->vb.sequence = sequence;
	buffer->vb.field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(vb, 0, sizeof(*meta));
	vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
}

static int zzh_mlx90640_capture_thread(void *private)
{
	struct zzh_mlx90640 *mlx = private;
	struct zzh_mlx90640_sample *first = &mlx->pair_samples[0];
	struct zzh_mlx90640_sample *next = &mlx->pair_samples[1];
	bool have_first = false;
	int ret;

	while (!kthread_should_stop()) {
		ret = zzh_mlx90640_capture_subpage(mlx, next, true, true);
		if (ret == -EINTR && kthread_should_stop())
			break;
		if (ret) {
			dev_err(&mlx->client->dev,
				"capture failed, propagating queue error: %d\n", ret);
			vb2_queue_error(&mlx->queue);
			break;
		}

		if (!have_first) {
			*first = *next;
			have_first = true;
			continue;
		}
		if (next->subpage_id == first->subpage_id) {
			mlx->duplicate_subpage++;
			*first = *next;
			continue;
		}

		zzh_mlx90640_complete_pair(mlx, first, next);
		have_first = false;
	}

	return 0;
}

static void zzh_mlx90640_return_buffers(struct zzh_mlx90640 *mlx,
					enum vb2_buffer_state state)
{
	struct zzh_mlx90640_buffer *buffer;
	struct zzh_mlx90640_buffer *tmp;
	unsigned long flags;

	spin_lock_irqsave(&mlx->slock, flags);
	list_for_each_entry_safe(buffer, tmp, &mlx->active_buffers, list) {
		list_del(&buffer->list);
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&mlx->slock, flags);
}

static int zzh_mlx90640_queue_setup(struct vb2_queue *queue,
				    unsigned int *num_buffers,
				    unsigned int *num_planes,
				    unsigned int sizes[],
				    struct device *alloc_devs[])
{
	unsigned int size = sizeof(struct zzh_mlx90640_meta_v1);

	if (*num_planes) {
		if (sizes[0] < size)
			return -EINVAL;
	} else {
		*num_planes = 1;
		sizes[0] = size;
	}
	if (queue->num_buffers + *num_buffers < 2)
		*num_buffers = 2 - queue->num_buffers;

	return 0;
}

static int zzh_mlx90640_buffer_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	unsigned int size = sizeof(struct zzh_mlx90640_meta_v1);

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	vbuf->field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(vb, 0, size);

	return 0;
}

static void zzh_mlx90640_buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct zzh_mlx90640 *mlx = vb2_get_drv_priv(vb->vb2_queue);
	struct zzh_mlx90640_buffer *buffer;
	unsigned long flags;

	buffer = container_of(vbuf, struct zzh_mlx90640_buffer, vb);
	spin_lock_irqsave(&mlx->slock, flags);
	list_add_tail(&buffer->list, &mlx->active_buffers);
	spin_unlock_irqrestore(&mlx->slock, flags);
}

static int zzh_mlx90640_start_streaming(struct vb2_queue *queue,
					unsigned int count)
{
	struct zzh_mlx90640 *mlx = vb2_get_drv_priv(queue);
	int ret;

	if (mlx->capture_thread)
		return -EBUSY;

	mlx->pair_sequence = 0;
	mlx->dropped_no_buffer = 0;
	mlx->duplicate_subpage = 0;
	ret = zzh_mlx90640_prepare_stream(mlx);
	if (ret)
		goto error_buffers;

	mlx->capture_thread = kthread_run(zzh_mlx90640_capture_thread, mlx,
					  "zzh-mlx90640-%d-%02x",
					  mlx->client->adapter->nr,
					  mlx->client->addr);
	if (IS_ERR(mlx->capture_thread)) {
		ret = PTR_ERR(mlx->capture_thread);
		mlx->capture_thread = NULL;
		zzh_mlx90640_restore_control(mlx);
		goto error_buffers;
	}

	dev_info(&mlx->client->dev,
		 "stream started: %u Hz subpages, control 0x%04x\n",
		 mlx->subpage_rate_hz, mlx->active_control);
	return 0;

error_buffers:
	zzh_mlx90640_return_buffers(mlx, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void zzh_mlx90640_stop_streaming(struct vb2_queue *queue)
{
	struct zzh_mlx90640 *mlx = vb2_get_drv_priv(queue);
	int restore_ret;

	if (mlx->capture_thread) {
		kthread_stop(mlx->capture_thread);
		mlx->capture_thread = NULL;
	}
	restore_ret = zzh_mlx90640_restore_control(mlx);
	if (restore_ret)
		vb2_queue_error(queue);
	zzh_mlx90640_return_buffers(mlx, VB2_BUF_STATE_ERROR);
	dev_info(&mlx->client->dev,
		 "stream stopped: pairs=%u no-buffer=%u duplicate=%u\n",
		 mlx->pair_sequence, mlx->dropped_no_buffer,
		 mlx->duplicate_subpage);
}

static const struct vb2_ops zzh_mlx90640_queue_ops = {
	.queue_setup = zzh_mlx90640_queue_setup,
	.buf_prepare = zzh_mlx90640_buffer_prepare,
	.buf_queue = zzh_mlx90640_buffer_queue,
	.start_streaming = zzh_mlx90640_start_streaming,
	.stop_streaming = zzh_mlx90640_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int zzh_mlx90640_querycap(struct file *file, void *priv,
				 struct v4l2_capability *capability)
{
	struct zzh_mlx90640 *mlx = video_drvdata(file);

	strscpy(capability->driver, ZZH_MLX90640_DRIVER_NAME,
		sizeof(capability->driver));
	strscpy(capability->card, mlx->video_dev.name,
		sizeof(capability->card));
	snprintf(capability->bus_info, sizeof(capability->bus_info),
		 "I2C:%d-%04x", mlx->client->adapter->nr, mlx->client->addr);

	return 0;
}

static int zzh_mlx90640_enum_fmt_meta(struct file *file, void *priv,
				      struct v4l2_fmtdesc *format)
{
	if (format->index || format->type != V4L2_BUF_TYPE_META_CAPTURE)
		return -EINVAL;
	format->pixelformat = V4L2_META_FMT_ZZH_MLX90640;

	return 0;
}

static int zzh_mlx90640_g_fmt_meta(struct file *file, void *priv,
				   struct v4l2_format *format)
{
	if (format->type != V4L2_BUF_TYPE_META_CAPTURE)
		return -EINVAL;
	memset(&format->fmt.meta, 0, sizeof(format->fmt.meta));
	format->fmt.meta.dataformat = V4L2_META_FMT_ZZH_MLX90640;
	format->fmt.meta.buffersize = sizeof(struct zzh_mlx90640_meta_v1);

	return 0;
}

static const struct v4l2_ioctl_ops zzh_mlx90640_ioctl_ops = {
	.vidioc_querycap = zzh_mlx90640_querycap,
	.vidioc_enum_fmt_meta_cap = zzh_mlx90640_enum_fmt_meta,
	.vidioc_g_fmt_meta_cap = zzh_mlx90640_g_fmt_meta,
	.vidioc_s_fmt_meta_cap = zzh_mlx90640_g_fmt_meta,
	.vidioc_try_fmt_meta_cap = zzh_mlx90640_g_fmt_meta,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations zzh_mlx90640_file_ops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.read = vb2_fop_read,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static int zzh_mlx90640_nvmem_read(void *private, unsigned int offset,
				   void *value, size_t bytes)
{
	struct zzh_mlx90640 *mlx = private;

	if (offset > sizeof(mlx->eeprom_be) ||
	    bytes > sizeof(mlx->eeprom_be) - offset)
		return -EINVAL;
	memcpy(value, &mlx->eeprom_be[offset], bytes);

	return 0;
}

static int zzh_mlx90640_cache_eeprom(struct zzh_mlx90640 *mlx)
{
	u16 *first;
	u16 *second;
	unsigned int i;
	int ret;

	first = kcalloc(ZZH_MLX90640_EEPROM_WORDS, sizeof(*first), GFP_KERNEL);
	second = kcalloc(ZZH_MLX90640_EEPROM_WORDS, sizeof(*second), GFP_KERNEL);
	if (!first || !second) {
		ret = -ENOMEM;
		goto out;
	}
	ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_EEPROM_START,
				      first, ZZH_MLX90640_EEPROM_WORDS);
	if (ret)
		goto out;
	ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_EEPROM_START,
				      second, ZZH_MLX90640_EEPROM_WORDS);
	if (ret)
		goto out;
	if (memcmp(first, second, ZZH_MLX90640_EEPROM_BYTES)) {
		ret = -EILSEQ;
		goto out;
	}
	for (i = 0; i < ZZH_MLX90640_EEPROM_WORDS; i++) {
		mlx->eeprom_be[i * 2] = first[i] >> 8;
		mlx->eeprom_be[i * 2 + 1] = first[i] & 0xff;
	}
	dev_info(&mlx->client->dev,
		 "EEPROM cached, reference ID %04x-%04x-%04x\n",
		 first[ZZH_MLX90640_ID_OFFSET],
		 first[ZZH_MLX90640_ID_OFFSET + 1],
		 first[ZZH_MLX90640_ID_OFFSET + 2]);
out:
	kfree(second);
	kfree(first);
	return ret;
}

static int zzh_mlx90640_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	struct zzh_mlx90640 *mlx;
	struct nvmem_device *nvmem;
	struct vb2_queue *queue;
	u16 status;
	u16 control;
	int ret;

	BUILD_BUG_ON(sizeof(struct zzh_mlx90640_meta_v1) !=
		     ZZH_MLX90640_META_V1_SIZE);
	BUILD_BUG_ON(offsetof(struct zzh_mlx90640_meta_v1, subpage) !=
		     ZZH_MLX90640_META_V1_HEADER_SIZE);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (client->addr != ZZH_MLX90640_I2C_ADDRESS)
		return -EINVAL;

	mlx = devm_kzalloc(&client->dev, sizeof(*mlx), GFP_KERNEL);
	if (!mlx)
		return -ENOMEM;
	mlx->client = client;
	mlx->subpage_rate_hz = 16;
	if (client->dev.of_node)
		of_property_read_u32(client->dev.of_node,
				     "zzh,subpage-rate-hz",
				     &mlx->subpage_rate_hz);
	if (zzh_mlx90640_rate_code(mlx->subpage_rate_hz) < 0) {
		dev_err(&client->dev,
			"zzh,subpage-rate-hz must be 8 or 16\n");
		return -EINVAL;
	}

	mutex_init(&mlx->lock);
	mutex_init(&mlx->queue_lock);
	spin_lock_init(&mlx->slock);
	INIT_LIST_HEAD(&mlx->active_buffers);
	i2c_set_clientdata(client, mlx);

	ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_STATUS, &status, 1);
	if (ret)
		return ret;
	ret = zzh_mlx90640_read_words(mlx, ZZH_MLX90640_CONTROL, &control, 1);
	if (ret)
		return ret;
	ret = zzh_mlx90640_cache_eeprom(mlx);
	if (ret) {
		dev_err(&client->dev, "EEPROM verification failed: %d\n", ret);
		return ret;
	}

	snprintf(mlx->nvmem_name, sizeof(mlx->nvmem_name),
		 "zzh_mlx90640_eeprom-%d-%02x",
		 client->adapter->nr, client->addr);
	mlx->nvmem_config.name = mlx->nvmem_name;
	mlx->nvmem_config.id = -1;
	mlx->nvmem_config.owner = THIS_MODULE;
	mlx->nvmem_config.dev = &client->dev;
	mlx->nvmem_config.read_only = true;
	mlx->nvmem_config.root_only = true;
	mlx->nvmem_config.reg_read = zzh_mlx90640_nvmem_read;
	mlx->nvmem_config.size = sizeof(mlx->eeprom_be);
	mlx->nvmem_config.word_size = 1;
	mlx->nvmem_config.stride = 1;
	mlx->nvmem_config.priv = mlx;
	nvmem = devm_nvmem_register(&client->dev, &mlx->nvmem_config);
	if (IS_ERR(nvmem))
		return dev_err_probe(&client->dev, PTR_ERR(nvmem),
				     "failed to register EEPROM nvmem\n");

	ret = v4l2_device_register(&client->dev, &mlx->v4l2_dev);
	if (ret)
		return ret;

	queue = &mlx->queue;
	queue->type = V4L2_BUF_TYPE_META_CAPTURE;
	queue->io_modes = VB2_MMAP | VB2_READ;
	queue->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	queue->drv_priv = mlx;
	queue->buf_struct_size = sizeof(struct zzh_mlx90640_buffer);
	queue->min_buffers_needed = 2;
	queue->ops = &zzh_mlx90640_queue_ops;
	queue->mem_ops = &vb2_vmalloc_memops;
	queue->lock = &mlx->queue_lock;
	queue->dev = &client->dev;
	ret = vb2_queue_init(queue);
	if (ret)
		goto unregister_v4l2;

	strscpy(mlx->video_dev.name, "zzh-mlx90640-meta",
		sizeof(mlx->video_dev.name));
	mlx->video_dev.v4l2_dev = &mlx->v4l2_dev;
	mlx->video_dev.fops = &zzh_mlx90640_file_ops;
	mlx->video_dev.ioctl_ops = &zzh_mlx90640_ioctl_ops;
	mlx->video_dev.release = video_device_release_empty;
	mlx->video_dev.lock = &mlx->lock;
	mlx->video_dev.queue = queue;
	mlx->video_dev.device_caps = V4L2_CAP_META_CAPTURE |
		V4L2_CAP_READWRITE | V4L2_CAP_STREAMING;
	mlx->video_dev.vfl_dir = VFL_DIR_RX;
	video_set_drvdata(&mlx->video_dev, mlx);

	ret = video_register_device(&mlx->video_dev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto release_queue;

	dev_info(&client->dev,
		 "registered Meta v1 (%u bytes), status=0x%04x control=0x%04x, %u Hz subpages\n",
		 ZZH_MLX90640_META_V1_SIZE, status, control,
		 mlx->subpage_rate_hz);
	return 0;

release_queue:
	vb2_queue_release(queue);
unregister_v4l2:
	v4l2_device_unregister(&mlx->v4l2_dev);
	return ret;
}

static int zzh_mlx90640_remove(struct i2c_client *client)
{
	struct zzh_mlx90640 *mlx = i2c_get_clientdata(client);

	if (mlx->capture_thread) {
		kthread_stop(mlx->capture_thread);
		mlx->capture_thread = NULL;
	}
	zzh_mlx90640_restore_control(mlx);
	vb2_queue_error(&mlx->queue);
	zzh_mlx90640_return_buffers(mlx, VB2_BUF_STATE_ERROR);
	video_unregister_device(&mlx->video_dev);
	vb2_queue_release(&mlx->queue);
	v4l2_device_unregister(&mlx->v4l2_dev);

	return 0;
}

static const struct of_device_id zzh_mlx90640_of_match[] = {
	{ .compatible = "zzh,mlx90640" },
	{ }
};
MODULE_DEVICE_TABLE(of, zzh_mlx90640_of_match);

static const struct i2c_device_id zzh_mlx90640_id[] = {
	{ "zzh_mlx90640", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, zzh_mlx90640_id);

static struct i2c_driver zzh_mlx90640_driver = {
	.driver = {
		.name = ZZH_MLX90640_DRIVER_NAME,
		.of_match_table = zzh_mlx90640_of_match,
	},
	.probe = zzh_mlx90640_probe,
	.remove = zzh_mlx90640_remove,
	.id_table = zzh_mlx90640_id,
};

module_i2c_driver(zzh_mlx90640_driver);

MODULE_AUTHOR("ZZH");
MODULE_DESCRIPTION("Independent MLX90640 V4L2 Meta/VB2 driver");
MODULE_LICENSE("GPL");
