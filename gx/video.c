/*
 * video - H.264 decoded by the host, for a guest that would do it itself.
 *
 * A browser decodes video with a copy of FFmpeg it carries, on the processor;
 * the Switch has a decoder in hardware and an FFmpeg of its own that drives
 * it (the "nvtegra" device). The guest's decoder is taken over where it is
 * handed its packets (guest/video.c): they come here, are decoded by the
 * host's FFmpeg, in hardware where it can, and the pictures are copied back
 * into the buffers the guest's player gave for them.
 *
 * These are functions of the host for the guest (host_symbol()), called on
 * the host's side of things: nothing here calls back into the guest.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include "gx.h"
#include "video.h"

#define MAX_DECODERS 8
#define MAX_WAITING 32

struct decoder {
	AVCodecContext *ctx;                   /* NULL when the slot is free */
	AVPacket *packet;
	AVFrame *waiting[MAX_WAITING];         /* decoded, not asked for yet */
	int head, count;
	AVFrame *current;                      /* the one last handed out */
	AVFrame *plain;                        /* it, out of the hardware's memory */
	int hardware, drained;
	long pictures;
};

static struct decoder decoders[MAX_DECODERS];
static struct host_lock *lock;

#ifdef __SWITCH__
static enum AVPixelFormat pick_format(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
	/* The hardware's own kind of picture if it is offered. */
	for (const enum AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; f++)
		if (*f == AV_PIX_FMT_NVTEGRA)
			return *f;
	return formats[0];
}
#endif

static struct decoder *decoder_of(long handle)
{
	return handle >= 0 && handle < MAX_DECODERS && decoders[handle].ctx ? &decoders[handle] : NULL;
}

/* Whether there is anything to gain: a host that would decode on the
 * processor like the guest is better left alone, unless told to (for trying
 * all this where there is no hardware). */
long gx_video_available(void)
{
#ifdef __SWITCH__
	return 1;
#else
	return getenv("GX_VIDEO") != NULL;
#endif
}

long gx_video_open(const uint8_t *extradata, long size)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	struct decoder *d = NULL;
	long handle = -1;

	if (!lock)
		lock = host_lock_new();
	host_lock_acquire(lock);
	for (int i = 0; i < MAX_DECODERS && !d; i++)
		if (!decoders[i].ctx) {
			d = &decoders[i];
			handle = i;
		}
	if (d && codec) {
		memset(d, 0, sizeof(*d));
		d->ctx = avcodec_alloc_context3(codec);
	}
	host_lock_release(lock);
	if (!d || !d->ctx)
		return -1;
	if (size > 0) {
		d->ctx->extradata = av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE);
		if (d->ctx->extradata) {
			memcpy(d->ctx->extradata, extradata, size);
			d->ctx->extradata_size = size;
		}
	}
	d->ctx->pkt_timebase = (AVRational){ 1, 1000000 };
#ifdef __SWITCH__
	{
		AVBufferRef *device = NULL;

		if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL, 0) >= 0) {
			d->ctx->hw_device_ctx = device;
			d->ctx->get_format = pick_format;
			d->hardware = 1;
		} else {
			host_log("gx: video: the hardware decoder cannot be had: on the processor\n");
		}
	}
#endif
	d->packet = av_packet_alloc();
	d->plain = av_frame_alloc();
	if (!d->packet || !d->plain || avcodec_open2(d->ctx, codec, NULL) < 0) {
		host_log("gx: video: the host's H.264 decoder does not open\n");
		gx_video_close(handle);
		return -1;
	}
	host_log("gx: video: H.264 is decoded by the host%s\n", d->hardware ? ", in hardware" : "");
	return handle;
}

/* Takes out of the decoder what it has ready, so that it takes more in. */
static void collect(struct decoder *d)
{
	while (d->count < MAX_WAITING) {
		AVFrame *frame = av_frame_alloc();

		if (!frame || avcodec_receive_frame(d->ctx, frame) < 0) {
			av_frame_free(&frame);
			return;
		}
		d->waiting[(d->head + d->count++) % MAX_WAITING] = frame;
	}
}

/* A packet as the guest's player has it, or nothing (size 0) when there are
 * no more and what is left inside is wanted. */
long gx_video_send(long handle, const uint8_t *data, long size, int64_t pts)
{
	struct decoder *d = decoder_of(handle);
	int ret;

	if (!d)
		return -1;
	if (size <= 0) {
		if (!d->drained) {
			d->drained = 1;
			avcodec_send_packet(d->ctx, NULL);
			collect(d);
		}
		return 0;
	}
	d->drained = 0;
	if (av_new_packet(d->packet, size) < 0)
		return -1;
	memcpy(d->packet->data, data, size);
	d->packet->pts = pts;
	d->packet->dts = AV_NOPTS_VALUE;
	ret = avcodec_send_packet(d->ctx, d->packet);
	if (ret == AVERROR(EAGAIN)) {
		collect(d);
		ret = avcodec_send_packet(d->ctx, d->packet);
	}
	av_packet_unref(d->packet);
	collect(d);
	return ret < 0 && ret != AVERROR(EAGAIN) ? -1 : 0;
}

long gx_video_receive(long handle, struct gx_video_frame *out)
{
	struct decoder *d = decoder_of(handle);
	AVFrame *frame;

	if (!d)
		return -1;
	if (!d->count)
		return 0;
	av_frame_free(&d->current);
	frame = d->current = d->waiting[d->head];
	d->head = (d->head + 1) % MAX_WAITING;
	d->count--;
	out->width = frame->width;
	out->height = frame->height;
	out->pts = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
#if LIBAVUTIL_VERSION_MAJOR >= 58
	out->key = !!(frame->flags & AV_FRAME_FLAG_KEY);
#else
	out->key = frame->key_frame;
#endif
	out->range = frame->color_range;
	out->primaries = frame->color_primaries;
	out->transfer = frame->color_trc;
	out->space = frame->colorspace;
	if (!d->pictures++)
		host_log("gx: video: first picture, %dx%d (%s)\n", frame->width, frame->height,
			 av_get_pix_fmt_name(frame->format));
	return 1;
}

/* The picture last received, into three planes of the guest's: Y, U and V,
 * the last two at half the size each way. */
long gx_video_copy(long handle, uint8_t *y, long y_stride, uint8_t *u, long u_stride,
		   uint8_t *v, long v_stride)
{
	struct decoder *d = decoder_of(handle);
	const AVFrame *frame = d ? d->current : NULL;
	int width, height;

	if (!frame)
		return -1;
	if (frame->hw_frames_ctx) {
		/* Out of the decoder's memory first. */
		av_frame_unref(d->plain);
		if (av_hwframe_transfer_data(d->plain, frame, 0) < 0)
			return -1;
		frame = d->plain;
	}
	width = frame->width;
	height = frame->height;
	for (int row = 0; row < height; row++)
		memcpy(y + row * y_stride, frame->data[0] + row * frame->linesize[0], width);
	if (frame->format == AV_PIX_FMT_NV12) {
		/* U and V come interleaved from the hardware. */
		for (int row = 0; row < (height + 1) / 2; row++) {
			const uint8_t *uv = frame->data[1] + row * frame->linesize[1];
			uint8_t *up = u + row * u_stride, *vp = v + row * v_stride;

			for (int x = 0; x < (width + 1) / 2; x++) {
				up[x] = uv[2 * x];
				vp[x] = uv[2 * x + 1];
			}
		}
	} else if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
		for (int row = 0; row < (height + 1) / 2; row++) {
			memcpy(u + row * u_stride, frame->data[1] + row * frame->linesize[1], (width + 1) / 2);
			memcpy(v + row * v_stride, frame->data[2] + row * frame->linesize[2], (width + 1) / 2);
		}
	} else {
		return -1;
	}
	return 0;
}

static void forget_pictures(struct decoder *d)
{
	while (d->count) {
		av_frame_free(&d->waiting[d->head]);
		d->head = (d->head + 1) % MAX_WAITING;
		d->count--;
	}
	av_frame_free(&d->current);
}

void gx_video_flush(long handle)
{
	struct decoder *d = decoder_of(handle);

	if (!d)
		return;
	avcodec_flush_buffers(d->ctx);
	forget_pictures(d);
	d->drained = 0;
}

void gx_video_close(long handle)
{
	struct decoder *d = handle >= 0 && handle < MAX_DECODERS ? &decoders[handle] : NULL;

	if (!d || !d->ctx)
		return;
	forget_pictures(d);
	av_frame_free(&d->plain);
	av_packet_free(&d->packet);
	avcodec_free_context(&d->ctx);
}

void *video_symbol(const char *name)
{
	static const struct {
		const char *name;
		void *fn;
	} ours[] = {
		{ "gx_video_available", gx_video_available }, { "gx_video_open", gx_video_open },
		{ "gx_video_send", gx_video_send },           { "gx_video_receive", gx_video_receive },
		{ "gx_video_copy", gx_video_copy },           { "gx_video_flush", gx_video_flush },
		{ "gx_video_close", gx_video_close },
	};

	for (size_t i = 0; i < sizeof(ours) / sizeof(ours[0]); i++)
		if (!strcmp(name, ours[i].name))
			return ours[i].fn;
	return NULL;
}
