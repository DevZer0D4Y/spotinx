/*
 * wl - the display a Chromium with a window draws on: a Wayland compositor
 * small enough to live in the host. See wl.c.
 */
#ifndef GX_WL_H
#define GX_WL_H

#include <stdint.h>

#define WL_SCREEN_WIDTH 1280
#define WL_SCREEN_HEIGHT 720

/* Opens the display (the socket /run/wayland-0 of the guest) and starts the
 * thread that serves it. To be called once the kernel runs and its /run is
 * there, before the guest starts; the guest finds the display with
 * XDG_RUNTIME_DIR=/run and WAYLAND_DISPLAY=wayland-0. */
int wl_start(void);

/* What the host provides, called from the one thread of wl.c, together with
 * host_input() of cdp.h for what the user does. */

/* The window's picture: rows of stride bytes, four per pixel in the order
 * blue, green, red, unused. Valid until the call returns. */
void host_present(const uint8_t *pixels, int width, int height, int stride);

#endif
