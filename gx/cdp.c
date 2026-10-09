/*
 * cdp.c - a screen and inputs for a headless Chromium.
 *
 * chromium-headless-shell has no window, but it can be driven through the
 * Chrome DevTools Protocol: with --remote-debugging-pipe it reads commands on
 * its descriptor 3 and writes answers and events on 4, as JSON texts ended by
 * a zero byte. One host thread holds the other ends of those pipes (they are
 * the kernel's, so it talks through LKL like the guest), asks for a
 * "screencast" of the page, hands each picture to the host to show, and
 * turns what the user does into input events of the protocol.
 *
 * Only what is needed is read out of the JSON, by looking for the keys: the
 * messages are Chromium's own and their shape is known.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lkl.h>
#include "gx.h"
#include "cdp.h"

#define MESSAGE_MAX (8UL << 20)
#define HOST_READ_FD 250    /* our ends, out of the way of the guest's */
#define HOST_WRITE_FD 251

static int next_id = 1;
static char session[128];   /* the page we are attached to, once we are */

/*
 * How smooth it is, written to the log every few seconds: how many pictures
 * came, how large, and where the time of each went. "pipe" is from the first
 * byte of a picture to its last, "show" is the host decoding and displaying
 * it, and "wait" is from our acknowledgement (sent as soon as a picture is
 * complete) to the first byte of the next one, which is Chromium drawing and
 * compressing when the page is changing (and nothing at all when it is not:
 * the longest waits are the user reading).
 */
#define STATS_PERIOD_NS 5000000000ULL

int cdp_frames_per_second;

static struct {
	uint64_t since, second_since;
	uint64_t first_byte, acked;
	int frames, second_frames, inputs;
	uint64_t bytes, pipe_ns, show_ns, show_max, wait_ns, wait_max;
} stats;

static void stats_frame(size_t bytes, uint64_t complete, uint64_t shown)
{
	uint64_t wait = stats.acked && stats.first_byte > stats.acked ? stats.first_byte - stats.acked : 0;

	stats.frames++;
	stats.second_frames++;
	stats.bytes += bytes;
	stats.pipe_ns += complete - stats.first_byte;
	stats.show_ns += shown - complete;
	if (shown - complete > stats.show_max)
		stats.show_max = shown - complete;
	stats.wait_ns += wait;
	if (wait > stats.wait_max)
		stats.wait_max = wait;
	stats.acked = complete;
}

static void stats_tick(uint64_t now)
{
	if (!stats.since)
		stats.since = stats.second_since = now;
	if (now - stats.second_since >= 1000000000ULL) {
		cdp_frames_per_second = stats.second_frames;
		stats.second_frames = 0;
		stats.second_since = now;
	}
	if (now - stats.since < STATS_PERIOD_NS)
		return;
	if (stats.frames) {
		int n = stats.frames;

		host_log("gx: %d pictures in 5 s (%d.%d per second), %lu KB each; per picture: "
			 "pipe %lu ms, show %lu ms (%lu at most), wait %lu ms (%lu at most); %d inputs\n",
			 n, n / 5, n * 2 % 10, (unsigned long)(stats.bytes / n >> 10),
			 (unsigned long)(stats.pipe_ns / n / 1000000),
			 (unsigned long)(stats.show_ns / n / 1000000),
			 (unsigned long)(stats.show_max / 1000000),
			 (unsigned long)(stats.wait_ns / n / 1000000),
			 (unsigned long)(stats.wait_max / 1000000), stats.inputs);
	}
	stats.since = now;
	stats.frames = stats.inputs = 0;
	stats.bytes = stats.pipe_ns = stats.show_ns = stats.show_max = 0;
	stats.wait_ns = stats.wait_max = 0;
}

static void send_text(const char *text, size_t len)
{
	for (size_t done = 0; done < len + 1;) {   /* with its final zero */
		long n = lkl_sys_write(HOST_WRITE_FD, text + done, len + 1 - done);

		if (n <= 0)
			return;
		done += n;
	}
}

/* Sends a command to the page (or to the browser before there is a page). */
static void command(const char *method, const char *params_fmt, ...)
{
	char params[1024] = "{}", text[1400];
	va_list ap;
	int n;

	if (params_fmt) {
		va_start(ap, params_fmt);
		vsnprintf(params, sizeof(params), params_fmt, ap);
		va_end(ap);
	}
	if (session[0])
		n = snprintf(text, sizeof(text), "{\"id\":%d,\"sessionId\":\"%s\",\"method\":\"%s\",\"params\":%s}",
			     next_id++, session, method, params);
	else
		n = snprintf(text, sizeof(text), "{\"id\":%d,\"method\":\"%s\",\"params\":%s}",
			     next_id++, method, params);
	if (n > 0 && n < (int)sizeof(text))
		send_text(text, n);
}

/* Copies the string value of "key" out of a message; returns its length, or
 * -1. Enough for values without escapes: identifiers, base64. */
static long string_value(const char *message, const char *key, char *out, size_t room)
{
	char pattern[64];
	const char *start, *end;

	snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	start = strstr(message, pattern);
	if (!start)
		return -1;
	start += strlen(pattern);
	end = strchr(start, '"');
	if (!end || (size_t)(end - start) >= room)
		return -1;
	memcpy(out, start, end - start);
	out[end - start] = 0;
	return end - start;
}

/* Decodes base64 in place; returns the number of bytes. */
static size_t base64_decode(char *text, size_t len)
{
	static signed char value[256];
	uint8_t *out = (uint8_t *)text;
	uint32_t bits = 0;
	int count = 0;

	if (!value['B']) {
		static const char alphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		memset(value, -1, sizeof(value));
		for (int i = 0; i < 64; i++)
			value[(unsigned char)alphabet[i]] = i;
	}
	for (size_t i = 0; i < len; i++) {
		int v = value[(unsigned char)text[i]];

		if (v < 0)
			continue;
		bits = bits << 6 | v;
		if (++count == 4) {
			*out++ = bits >> 16;
			*out++ = bits >> 8;
			*out++ = bits;
			count = 0;
		}
	}
	if (count == 3) {
		*out++ = bits >> 10;
		*out++ = bits >> 2;
	} else if (count == 2) {
		*out++ = bits >> 4;
	}
	return out - (uint8_t *)text;
}

/* Writes text as the inside of a JSON string. */
static void json_escape(char *out, size_t room, const char *text)
{
	size_t n = 0;

	for (; *text && n + 7 < room; text++) {
		unsigned char c = *text;

		if (c == '"' || c == '\\') {
			out[n++] = '\\';
			out[n++] = c;
		} else if (c < 0x20) {
			n += snprintf(out + n, room - n, "\\u%04x", c);
		} else {
			out[n++] = c;
		}
	}
	out[n] = 0;
}

static void handle_message(char *message, size_t len)
{
	static char frame_session[32];
	char *data;

	if (!session[0]) {
		/* The answer to Target.getTargets, or to Target.attachToTarget. */
		char target[128];
		const char *page = strstr(message, "\"type\":\"page\"");

		if (string_value(message, "sessionId", session, sizeof(session)) > 0) {
			command("Page.enable", NULL);
			command("Page.startScreencast",
				"{\"format\":\"jpeg\",\"quality\":70,\"maxWidth\":1280,\"maxHeight\":720}");
		} else if (page) {
			/* The identifier comes first in each description. */
			const char *start = page;

			while (start > message && strncmp(start, "\"targetId\":\"", 12))
				start--;
			if (string_value(start, "targetId", target, sizeof(target)) > 0)
				command("Target.attachToTarget", "{\"targetId\":\"%s\",\"flatten\":true}",
					target);
		}
		return;
	}
	if (!strstr(message, "\"method\":\"Page.screencastFrame\""))
		return;
	/* The picture, and the number to acknowledge it with (the "sessionId"
	 * inside the parameters, a number, unlike the page's which is text). */
	data = strstr(message, "\"data\":\"");
	if (data) {
		const char *number = strstr(message, "\"sessionId\":");
		char *end;

		while (number && number[12] == '"')
			number = strstr(number + 12, "\"sessionId\":");
		data += 8;
		end = strchr(data, '"');
		if (number)
			snprintf(frame_session, sizeof(frame_session), "%d", atoi(number + 12));
		if (end) {
			uint64_t complete = host_now_ns();
			size_t bytes;

			/* Acknowledged before it is shown: Chromium then draws the
			 * next picture while this one is decoded and displayed,
			 * instead of after. */
			command("Page.screencastFrameAck", "{\"sessionId\":%s}", frame_session);
			bytes = base64_decode(data, end - data);
			host_frame((const uint8_t *)data, bytes);
			stats_frame(bytes, complete, host_now_ns());
		}
	}
}

static void handle_input(const struct cdp_input *in)
{
	char text[768];

	switch (in->type) {
	case CDP_TOUCH_DOWN:
	case CDP_TOUCH_MOVE:
		command("Input.dispatchTouchEvent", "{\"type\":\"%s\",\"touchPoints\":[{\"x\":%d,\"y\":%d}]}",
			in->type == CDP_TOUCH_DOWN ? "touchStart" : "touchMove", in->x, in->y);
		break;
	case CDP_TOUCH_UP:
		command("Input.dispatchTouchEvent", "{\"type\":\"touchEnd\",\"touchPoints\":[]}");
		break;
	case CDP_SCROLL:
		command("Input.dispatchMouseEvent",
			"{\"type\":\"mouseWheel\",\"x\":640,\"y\":360,\"deltaX\":%d,\"deltaY\":%d}",
			in->x, in->y);
		break;
	case CDP_BACK:
		command("Runtime.evaluate", "{\"expression\":\"history.back()\"}");
		break;
	case CDP_RELOAD:
		command("Page.reload", NULL);
		break;
	case CDP_NAVIGATE:
		json_escape(text, sizeof(text), in->text);
		command("Page.navigate", "{\"url\":\"%s\"}", text);
		break;
	case CDP_TEXT:
		json_escape(text, sizeof(text), in->text);
		command("Input.insertText", "{\"text\":\"%s\"}", text);
		break;
	case CDP_ENTER:
		command("Input.dispatchKeyEvent",
			"{\"type\":\"keyDown\",\"key\":\"Enter\",\"code\":\"Enter\","
			"\"windowsVirtualKeyCode\":13,\"text\":\"\\r\"}");
		command("Input.dispatchKeyEvent",
			"{\"type\":\"keyUp\",\"key\":\"Enter\",\"code\":\"Enter\",\"windowsVirtualKeyCode\":13}");
		break;
	case CDP_QUIT:
		session[0] = 0;   /* a command of the browser, not of the page */
		command("Browser.close", NULL);
		break;
	}
}

static void cdp_thread(void *arg)
{
	char *message = malloc(MESSAGE_MAX);
	size_t len = 0;
	int waited = 0;

	if (!message)
		return;
	for (;;) {
		struct lkl_pollfd pfd = { .fd = HOST_READ_FD, .events = LKL_POLLIN };
		struct cdp_input in;

		/* Short waits: what the user does is looked at in between. */
		if (lkl_sys_poll(&pfd, 1, 10) > 0) {
			long n = lkl_sys_read(HOST_READ_FD, message + len, MESSAGE_MAX - 1 - len);

			if (n <= 0)
				break;   /* Chromium is gone */
			if (!len)
				stats.first_byte = host_now_ns();
			len += n;
			for (;;) {
				char *end = memchr(message, 0, len);
				size_t used;

				if (!end)
					break;
				used = end - message + 1;
				handle_message(message, used - 1);
				memmove(message, message + used, len - used);
				len -= used;
				if (len)
					stats.first_byte = host_now_ns();
			}
			if (len == MESSAGE_MAX - 1)
				len = 0;   /* not a message we can do anything with */
		}
		if (!session[0]) {
			/* Until the page exists, keep asking where it is. */
			if (waited++ % 50 == 0)
				command("Target.getTargets", NULL);
			continue;
		}
		while (host_input(&in)) {
			handle_input(&in);
			stats.inputs++;
		}
		stats_tick(host_now_ns());
	}
	free(message);
}

int cdp_start(void)
{
	int to_guest[2], from_guest[2];
	long args[6] = { 0 };

	if (lkl_sys_pipe(to_guest) || lkl_sys_pipe(from_guest))
		return -1;
	/* Chromium reads commands on 3 and writes on 4; our ends go far away
	 * and the numbers the pipes were born with are given back. */
	static const int moves[4][2] = { { 0, 3 }, { 3, 4 }, { 1, HOST_WRITE_FD }, { 2, HOST_READ_FD } };
	int fds[4] = { to_guest[0], to_guest[1], from_guest[0], from_guest[1] };
	int moved[4];

	/* First out of the way, so that no move lands on a descriptor that is
	 * still to be moved. */
	for (int i = 0; i < 4; i++) {
		args[0] = fds[i];
		args[1] = 260 + i;
		args[2] = 0;
		if (lkl_syscall(__lkl__NR_dup3, args) < 0)
			return -1;
		lkl_sys_close(fds[i]);
		moved[i] = 260 + i;
	}
	for (int i = 0; i < 4; i++) {
		args[0] = moved[moves[i][0]];
		args[1] = moves[i][1];
		args[2] = 0;
		if (lkl_syscall(__lkl__NR_dup3, args) < 0)
			return -1;
	}
	for (int i = 0; i < 4; i++)
		lkl_sys_close(moved[i]);
	return host_thread_start(cdp_thread, NULL);
}
