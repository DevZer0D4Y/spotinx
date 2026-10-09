/*
 * cdp - a screen and inputs for a headless Chromium, over its remote
 * debugging pipe. See cdp.c.
 */
#ifndef GX_CDP_H
#define GX_CDP_H

#include <stddef.h>
#include <stdint.h>

enum {
	CDP_NONE,
	CDP_TOUCH_DOWN,   /* x, y: a finger on the page */
	CDP_TOUCH_MOVE,
	CDP_TOUCH_UP,
	CDP_SCROLL,       /* y: pixels, down if positive; x likewise */
	CDP_BACK,
	CDP_RELOAD,
	CDP_NAVIGATE,     /* text: a URL */
	CDP_TEXT,         /* text: typed into whatever has the focus */
	CDP_ENTER,
	CDP_QUIT,
};

struct cdp_input {
	int type;
	int x, y;
	char text[512];
};

/* Makes the pipe Chromium is told about with --remote-debugging-pipe (its
 * descriptors 3 and 4) and starts the thread at the other end. To be called
 * once the kernel runs and before the guest does. */
int cdp_start(void);

/* Pictures shown during the last whole second, for a host that wants to
 * display it. */
extern int cdp_frames_per_second;

/* What the host provides. Both are called from the one thread of cdp.c. */

/* A new picture of the page, as a JPEG. */
void host_frame(const uint8_t *jpeg, size_t len);
/* The next thing the user did, if any: returns 0 when there is nothing. */
int host_input(struct cdp_input *in);

#endif
