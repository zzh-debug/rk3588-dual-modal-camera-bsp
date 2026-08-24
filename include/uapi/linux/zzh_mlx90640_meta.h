/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ZZH_MLX90640_META_H
#define _UAPI_ZZH_MLX90640_META_H

#include <linux/types.h>
#include <linux/videodev2.h>

#define V4L2_META_FMT_ZZH_MLX90640 v4l2_fourcc('Z', 'M', 'L', 'X')

#define ZZH_MLX90640_META_MAGIC          0x584c4d5aU /* "ZMLX" in LE */
#define ZZH_MLX90640_META_VERSION        1
#define ZZH_MLX90640_META_SUBPAGES       2
#define ZZH_MLX90640_META_PIXEL_WORDS    768
#define ZZH_MLX90640_META_AUX_WORDS      64
#define ZZH_MLX90640_META_V1_HEADER_SIZE 24
#define ZZH_MLX90640_META_V1_SIZE        3400

#define ZZH_MLX90640_META_FLAG_CHESS       0x00000001U
#define ZZH_MLX90640_META_FLAG_CONSECUTIVE 0x00000002U

/*
 * All multi-byte fields are little-endian. The two subpages are ordered by
 * capture time; use subpage_id rather than array position to identify 0/1.
 */
struct zzh_mlx90640_subpage_v1 {
	__le64 ready_timestamp_ns;
	__le64 read_done_timestamp_ns;
	__le16 status;
	__le16 control;
	__le16 subpage_id;
	__le16 reserved;
	__le16 pixels[ZZH_MLX90640_META_PIXEL_WORDS];
	__le16 auxiliary[ZZH_MLX90640_META_AUX_WORDS];
};

struct zzh_mlx90640_meta_v1 {
	__le32 magic;
	__le16 version;
	__le16 header_size;
	__le32 buffer_size;
	__le32 pair_sequence;
	__le32 flags;
	__le32 reserved;
	struct zzh_mlx90640_subpage_v1
		subpage[ZZH_MLX90640_META_SUBPAGES];
};

#endif /* _UAPI_ZZH_MLX90640_META_H */
