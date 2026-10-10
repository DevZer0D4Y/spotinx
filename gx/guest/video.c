/*
 * video.c - libgxvideo.so, loaded ahead of a browser (LD_PRELOAD) to have its
 * H.264 decoded by the host. See ../video.c for the host's side.
 *
 * Chromium carries its own FFmpeg, inside its one binary and without names.
 * What it does have, like any FFmpeg, is a description of each decoder: its
 * name, what it can do, and the functions that do it. The one of the H.264
 * decoder is found by what it says (its two names and its number), and two
 * of its functions are replaced: the one that is handed each packet, which
 * now hands it to the host and returns the picture the host gives back, in a
 * buffer asked from the player as the decoder itself would; and the one that
 * forgets everything when the player seeks. Two abilities are taken off its
 * description: working on several threads, since there is nothing left for
 * them to do, and the one that goes with asking the player for buffers
 * through FFmpeg's own function, which cannot be reached from here (the
 * player's function is called directly instead).
 *
 * Where things are in FFmpeg's structures is as in the FFmpeg that Chromium
 * 154 carries (libavcodec 63.8, as its sources are built for Linux on arm64):
 * they are checked for what can be checked, and nothing is touched if the
 * description is not found where its names lead.
 */
#define _GNU_SOURCE
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The host's (hostlib.c makes them). */
long gx_video_available(void);
long gx_video_open(const uint8_t *extradata, long size);
long gx_video_send(long handle, const uint8_t *data, long size, int64_t pts);
long gx_video_receive(long handle, void *frame);
long gx_video_copy(long handle, uint8_t *y, long y_stride, uint8_t *u, long u_stride,
		   uint8_t *v, long v_stride);
void gx_video_flush(long handle);
void gx_video_close(long handle);

struct picture {
	int32_t width, height;
	int64_t pts;
	int32_t key;
	int32_t range, primaries, transfer, space;
};

/* FFCodec: the description of a decoder. */
#define CODEC_TYPE 16
#define CODEC_ID 20
#define CODEC_CAPABILITIES 24
#define CODEC_INIT 88
#define CODEC_DECODE 96
#define CODEC_CLOSE 104
#define CODEC_FLUSH 112
#define CODEC_SIZE 176
#define ID_H264 27
#define CAP_DR1 0x0002
#define CAP_DELAY 0x0020
#define CAP_THREADS 0x3000   /* frame threads and slice threads */

/* AVCodecContext, AVFrame and AVPacket. */
#define CTX_EXTRADATA 72
#define CTX_EXTRADATA_SIZE 80
#define CTX_WIDTH 112
#define CTX_HEIGHT 116
#define CTX_CODED_WIDTH 120
#define CTX_CODED_HEIGHT 124
#define CTX_PIX_FMT 136
#define CTX_GET_BUFFER2 408
#define FRAME_DATA 0
#define FRAME_LINESIZE 64
#define FRAME_WIDTH 104
#define FRAME_HEIGHT 108
#define FRAME_FORMAT 116
#define FRAME_PTS 136
#define FRAME_FLAGS 276
#define FRAME_RANGE 280
#define FRAME_PRIMARIES 284
#define FRAME_TRANSFER 288
#define FRAME_SPACE 292
#define PACKET_PTS 8
#define PACKET_DATA 24
#define PACKET_SIZE 32
#define PIX_FMT_YUV420P 0
#define FRAME_FLAG_KEY 2
#define ERROR_INVALID_DATA (-0x41444E49)   /* AVERROR_INVALIDDATA */

#define FIELD(type, base, offset) (*(type *)((char *)(base) + (offset)))

#define MAX_SESSIONS 8

static struct {
	void *ctx;       /* the player's decoder: NULL when the slot is free */
	long handle;     /* the host's */
} sessions[MAX_SESSIONS];

static int (*original_decode)(void *ctx, void *frame, int *got, void *packet);
static void (*original_flush)(void *ctx);
static int (*original_close)(void *ctx);

static void say(const char *text)
{
	(void)!write(2, text, strlen(text));
}

static long session_of(void *ctx, int make)
{
	int spare = -1;

	for (int i = 0; i < MAX_SESSIONS; i++) {
		if (sessions[i].ctx == ctx)
			return sessions[i].handle;
		if (!sessions[i].ctx && spare < 0)
			spare = i;
	}
	if (!make || spare < 0)
		return -1;
	sessions[spare].handle = gx_video_open(FIELD(const uint8_t *, ctx, CTX_EXTRADATA),
					       FIELD(int32_t, ctx, CTX_EXTRADATA_SIZE));
	if (sessions[spare].handle < 0)
		return -1;
	sessions[spare].ctx = ctx;
	return sessions[spare].handle;
}

/* A packet in, and a picture out if the host has one ready: not the picture
 * of that packet, pictures come out later than they go in and in the order
 * they are shown. An empty packet asks for what is left. */
static int decode(void *ctx, void *frame, int *got, void *packet)
{
	int (*get_buffer)(void *, void *, int) = FIELD(void *, ctx, CTX_GET_BUFFER2);
	int size = FIELD(int32_t, packet, PACKET_SIZE);
	long handle = session_of(ctx, 1);
	struct picture p;
	int ret;

	*got = 0;
	if (handle < 0 || !get_buffer)
		return original_decode(ctx, frame, got, packet);
	if (gx_video_send(handle, FIELD(const uint8_t *, packet, PACKET_DATA), size,
			  FIELD(int64_t, packet, PACKET_PTS)) < 0)
		return ERROR_INVALID_DATA;
	if (gx_video_receive(handle, &p) != 1)
		return size;

	FIELD(int32_t, frame, FRAME_FORMAT) = PIX_FMT_YUV420P;
	FIELD(int32_t, frame, FRAME_WIDTH) = p.width;
	FIELD(int32_t, frame, FRAME_HEIGHT) = p.height;
	FIELD(int32_t, ctx, CTX_PIX_FMT) = PIX_FMT_YUV420P;
	FIELD(int32_t, ctx, CTX_WIDTH) = FIELD(int32_t, ctx, CTX_CODED_WIDTH) = p.width;
	FIELD(int32_t, ctx, CTX_HEIGHT) = FIELD(int32_t, ctx, CTX_CODED_HEIGHT) = p.height;
	ret = get_buffer(ctx, frame, 0);
	if (ret < 0)
		return ret;
	{
		uint8_t **data = &FIELD(uint8_t *, frame, FRAME_DATA);
		int32_t *linesize = &FIELD(int32_t, frame, FRAME_LINESIZE);

		if (gx_video_copy(handle, data[0], linesize[0], data[1], linesize[1], data[2],
				  linesize[2]) < 0)
			return ERROR_INVALID_DATA;
	}
	FIELD(int64_t, frame, FRAME_PTS) = p.pts;
	if (p.key)
		FIELD(int32_t, frame, FRAME_FLAGS) |= FRAME_FLAG_KEY;
	FIELD(int32_t, frame, FRAME_RANGE) = p.range;
	FIELD(int32_t, frame, FRAME_PRIMARIES) = p.primaries;
	FIELD(int32_t, frame, FRAME_TRANSFER) = p.transfer;
	FIELD(int32_t, frame, FRAME_SPACE) = p.space;
	*got = 1;
	return size;
}

static void flush(void *ctx)
{
	long handle = session_of(ctx, 0);

	if (handle >= 0)
		gx_video_flush(handle);
	original_flush(ctx);
}

static int close_decoder(void *ctx)
{
	for (int i = 0; i < MAX_SESSIONS; i++)
		if (sessions[i].ctx == ctx) {
			gx_video_close(sessions[i].handle);
			sessions[i].ctx = NULL;
		}
	return original_close(ctx);
}

/* Whether p, somewhere in the program's data, is a pointer to this text. */
static int points_to(const char *start, const char *end, const void *p, const char *text)
{
	const char *s = *(const char *const *)p;
	size_t len = strlen(text) + 1;

	return s >= start && s + len <= end && !memcmp(s, text, len);
}

static int look(struct dl_phdr_info *info, size_t size, void *unused)
{
	const char *start = (const char *)-1, *end = NULL;

	(void)size;
	(void)unused;
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];

		if (ph->p_type != PT_LOAD)
			continue;
		if ((const char *)info->dlpi_addr + ph->p_vaddr < start)
			start = (const char *)info->dlpi_addr + ph->p_vaddr;
		if ((const char *)info->dlpi_addr + ph->p_vaddr + ph->p_memsz > end)
			end = (const char *)info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
	}
	/* The description is data that was made read-only once its pointers
	 * were set: in what the program says is such. */
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		char *from = (char *)info->dlpi_addr + ph->p_vaddr, *to = from + ph->p_memsz;

		if (ph->p_type != PT_GNU_RELRO)
			continue;
		for (char *p = (char *)(((uintptr_t)from + 7) & ~7UL); p + CODEC_SIZE <= to; p += 8) {
			long page = sysconf(_SC_PAGESIZE);
			char *first, *last;
			int capabilities;

			if (FIELD(int32_t, p, CODEC_ID) != ID_H264 || FIELD(int32_t, p, CODEC_TYPE) != 0 ||
			    !points_to(start, end, p, "h264") ||
			    !points_to(start, end, p + 8, "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10"))
				continue;
			capabilities = FIELD(int32_t, p, CODEC_CAPABILITIES);
			if (!(capabilities & CAP_DR1) || !(capabilities & CAP_DELAY) ||
			    !FIELD(void *, p, CODEC_DECODE) || !FIELD(void *, p, CODEC_FLUSH) ||
			    !FIELD(void *, p, CODEC_CLOSE) || !FIELD(void *, p, CODEC_INIT)) {
				say("gx: video: the H.264 decoder is not made as expected: left alone\n");
				return 1;
			}
			first = (char *)((uintptr_t)p & ~(page - 1));
			last = (char *)(((uintptr_t)p + CODEC_SIZE + page - 1) & ~(page - 1));
			if (mprotect(first, last - first, PROT_READ | PROT_WRITE)) {
				say("gx: video: the H.264 decoder cannot be changed: left alone\n");
				return 1;
			}
			original_decode = FIELD(void *, p, CODEC_DECODE);
			original_flush = FIELD(void *, p, CODEC_FLUSH);
			original_close = FIELD(void *, p, CODEC_CLOSE);
			FIELD(void *, p, CODEC_DECODE) = decode;
			FIELD(void *, p, CODEC_FLUSH) = flush;
			FIELD(void *, p, CODEC_CLOSE) = close_decoder;
			/* Not on several threads, and not said to ask the player
			 * for its buffers "itself": FFmpeg then expects on each
			 * picture something of its own that only its inside can
			 * put there, and stops the program when it is missing. */
			FIELD(int32_t, p, CODEC_CAPABILITIES) = capabilities & ~(CAP_THREADS | CAP_DR1);
			mprotect(first, last - first, PROT_READ);
			say("gx: video: the browser's H.264 decoder is the host's\n");
			return 1;
		}
	}
	return 1;   /* the program itself comes first, and is all there is to look at */
}

__attribute__((constructor)) static void take_over(void)
{
	if (gx_video_available())
		dl_iterate_phdr(look, NULL);
}
