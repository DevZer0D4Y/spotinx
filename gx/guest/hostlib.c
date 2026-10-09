/*
 * hostlib.c - a guest library whose functions are the host's.
 *
 * Built once per library (libEGL.so.1, libGLESv2.so.2) with GX_LIST naming a
 * file of GX_THUNK(function) lines. Each function is a stub that jumps to
 * gx_hostcall (tramp.S) with the address of the host's function in x16; the
 * arguments are left where the caller put them. The addresses come from gx
 * when the library is loaded.
 *
 * With GX_PROC_ADDRESS, the library also has eglGetProcAddress(), which hands
 * out spare stubs for functions that are not in any list (extensions).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GX_SYS_HOST_SYMBOL 0x47580001
#define GX_SYS_HOSTCALL 0x47580002

__attribute__((visibility("hidden"))) void *gx_hostcall_entry;

#define GX_THUNK(name)                                                          \
	__attribute__((visibility("hidden"))) void *gx_slot_##name;             \
	__asm__(".text\n"                                                       \
		".balign 4\n"                                                   \
		".globl " #name "\n"                                            \
		".type " #name ", %function\n"                                  \
		#name ":\n"                                                     \
		"	adrp x16, gx_slot_" #name "\n"                          \
		"	ldr x16, [x16, :lo12:gx_slot_" #name "]\n"              \
		"	adrp x17, gx_hostcall_entry\n"                          \
		"	ldr x17, [x17, :lo12:gx_hostcall_entry]\n"              \
		"	br x17\n"                                               \
		".size " #name ", .-" #name "\n");
#include GX_LIST
#undef GX_THUNK

#define GX_THUNK(name) { #name, &gx_slot_##name },
static const struct {
	const char *name;
	void **slot;
} thunks[] = {
#include GX_LIST
};
#undef GX_THUNK

__attribute__((constructor)) static void bind_thunks(void)
{
	void *missing = (void *)syscall(GX_SYS_HOSTCALL, 1);

	gx_hostcall_entry = (void *)syscall(GX_SYS_HOSTCALL, 0);
	for (size_t i = 0; i < sizeof(thunks) / sizeof(thunks[0]); i++) {
		void *fn = (void *)syscall(GX_SYS_HOST_SYMBOL, thunks[i].name);

		*thunks[i].slot = fn ? fn : missing;
	}
}

#ifdef GX_PROC_ADDRESS

/*
 * Stubs for the functions eglGetProcAddress() hands out, made ahead of time:
 * the guest cannot have memory that is both writable and executable, so they
 * are code like the others and only their slots are filled at run time.
 */
#define DYN_STUBS 1024
#define DYN_STUB_SIZE 20

__attribute__((visibility("hidden"))) void *gx_dyn_slots[DYN_STUBS];
extern const char gx_dyn_stubs[] __attribute__((visibility("hidden")));
__asm__(".text\n"
	".balign 4\n"
	".hidden gx_dyn_stubs\n"
	"gx_dyn_stubs:\n"
	".set gx_dyn_offset, 0\n"
	".rept 1024\n"
	"	adrp x16, gx_dyn_slots + gx_dyn_offset\n"
	"	ldr x16, [x16, :lo12:gx_dyn_slots + gx_dyn_offset]\n"
	"	adrp x17, gx_hostcall_entry\n"
	"	ldr x17, [x17, :lo12:gx_hostcall_entry]\n"
	"	br x17\n"
	".set gx_dyn_offset, gx_dyn_offset + 8\n"
	".endr\n");

static pthread_mutex_t dyn_lock = PTHREAD_MUTEX_INITIALIZER;
static char *dyn_names[DYN_STUBS];
static size_t dyn_count;

void (*GX_PROC_ADDRESS(const char *name))(void)
{
	/* A function one of the loaded thunk libraries exports is the answer. */
	void *found = dlsym(RTLD_DEFAULT, name);
	const char *stub = NULL;
	void *fn;

	if (found)
		return (void (*)(void))found;
	pthread_mutex_lock(&dyn_lock);
	for (size_t i = 0; i < dyn_count && !stub; i++)
		if (!strcmp(dyn_names[i], name))
			stub = gx_dyn_stubs + i * DYN_STUB_SIZE;
	if (!stub && dyn_count < DYN_STUBS && (fn = (void *)syscall(GX_SYS_HOST_SYMBOL, name)) &&
	    (dyn_names[dyn_count] = strdup(name))) {
		gx_dyn_slots[dyn_count] = fn;
		stub = gx_dyn_stubs + dyn_count * DYN_STUB_SIZE;
		dyn_count++;
	}
	pthread_mutex_unlock(&dyn_lock);
	return (void (*)(void))stub;
}

#endif
