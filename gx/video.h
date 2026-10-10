/*
 * video - H.264 decoded by the host, for a guest that would do it itself.
 * See video.c for the host's side and guest/video.c for the guest's.
 */
#ifndef GX_VIDEO_H
#define GX_VIDEO_H

#include <stdint.h>

/* What the host says of a picture it has decoded. */
struct gx_video_frame {
	int32_t width, height;
	int64_t pts;
	int32_t key;                               /* a picture that needs no other */
	int32_t range, primaries, transfer, space; /* its colours, as FFmpeg numbers them */
};

/* The host's functions for the guest, by name ("gx_video_..."): NULL if the
 * name is not one of them. For host_symbol(). */
void *video_symbol(const char *name);

long gx_video_available(void);
long gx_video_open(const uint8_t *extradata, long size);
long gx_video_send(long handle, const uint8_t *data, long size, int64_t pts);
long gx_video_receive(long handle, struct gx_video_frame *out);
long gx_video_copy(long handle, uint8_t *y, long y_stride, uint8_t *u, long u_stride,
		   uint8_t *v, long v_stride);
void gx_video_flush(long handle);
void gx_video_close(long handle);

#endif
