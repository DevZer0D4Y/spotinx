/*
 * glwin - the windows a guest draws with the GPU, put together on the screen.
 * See glwin.c. For a host's EGL functions to call: they stand in for the ones
 * a guest calls on its window surfaces.
 */
#ifndef GX_GLWIN_H
#define GX_GLWIN_H

#include <stdint.h>

struct glwin_host {
	/* A function of the host's EGL or GLES by its name: the real one. */
	void *(*symbol)(const char *name);
	/* Makes the surface that is the screen, for a configuration. */
	void *(*screen)(void *dpy, void *config);
	/* Called with the screen drawn and current, before it is shown: for a
	 * host that wants to look at it. May be NULL. */
	void (*presented)(void *dpy, void *screen);
};

void glwin_init(const struct glwin_host *host);

/* eglCreateWindowSurface: guest_window is what the guest passed as its native
 * window. Returns the surface, or NULL. */
void *glwin_create(void *dpy, void *config, void *guest_window);

/* eglSwapBuffers and eglDestroySurface: 1 if done, 0 if it failed, -1 if the
 * surface is not a window's (the host's own function is to be called). */
int glwin_swap(void *dpy, void *surface);
int glwin_destroy(void *dpy, void *surface);

/* eglQuerySurface for the width and the height of a window: returns whether
 * *value was set. */
int glwin_query(void *surface, int32_t attribute, int32_t *value);

#endif
