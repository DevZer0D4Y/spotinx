/*
 * gmem.c - the guest's address space.
 *
 * LKL has no MMU, so mmap/munmap/mprotect/brk are ours. One arena is reserved
 * from the host and a byte per page says what is mapped there. File mappings
 * are private copies read through LKL. Executable file mappings get their
 * `svc #0` instructions patched as they are mapped, which is what lets ld.so
 * and dlopen() load libraries we never saw.
 *
 * Anonymous memory that is not code ("data": heaps, stacks) is always readable
 * and writable for the host, whatever protection the guest gives it, and is
 * committed 256 KB at a time when the guest commits pieces of a reservation.
 * The guest cannot tell: there are no signals here, so a protection it sets is
 * only ever a fault it does not want. The host can: its kernel keeps a record
 * per run of pages mapped alike, out of a store that runs out (Horizon stops
 * serving the process at about ten thousand), and an allocator that commits
 * its memory sixteen kilobytes at a time makes one run of each.
 *
 * Shared file mappings (MAP_SHARED) are views of one copy of the file, made
 * of memory the host can map at several addresses. The copy is written back
 * to the file when its last view goes, so that a later mapping finds it. On
 * a host without such memory, every view of a file is at the same address.
 */
#include <string.h>
#include <lkl.h>
#include "gx.h"

/* How much address space the guest gets. Reserving costs nothing, and some
 * programs reserve a lot (Chromium's allocators take tens of gigabytes). */
#ifndef GX_ARENA_SIZE
#define GX_ARENA_SIZE (4UL << 30)
#endif
#define ARENA_MAX_PAGES (GX_ARENA_SIZE / GX_PAGE)
#define ARENA_MIN (1UL << 30)
#define BRK_SIZE (256UL << 20)

#define PG_USED 0x80
#define PG_COMMITTED 0x40
#define PG_SHARED 0x20   /* a view of a shared file */
#define PG_COPY 0x10     /* the one copy of a shared file, where views cannot be told apart */
#define PG_HOLE 0x08     /* not ours: the host could not reserve it */
#define PG_PROT 0x07

#define SVC0 0xD4000001u
#define VENEER_SIZE 32
#define BRANCH_RANGE (128L << 20)
#define MAX_IMAGES 1024
#define DATA_UNIT (256UL << 10)   /* what data is committed by, inside reservations */
#define DATA_PROT (GX_PROT_READ | GX_PROT_WRITE)
#define MAX_SHARED 512
#define MAX_VIEWS 4096
#define MAX_CHUNKS 256
#define KEEP_MAX 24            /* files kept without a view before a clean-up */
#define RAMFS_MAGIC 0x858458f6L
#define CHUNK_MAX_PAGES 4096   /* 16 MB */
#define MADV_DONTNEED 4

/* An image is one non-fixed file mapping (what ld.so makes first for each
 * library) followed by the pool its veneers are taken from. */
struct image {
	uint64_t start, end;
	uint64_t pool, pool_end, pool_next;
};

/*
 * A piece of the host's shared memory. Hosts only map such a piece whole, and
 * may have few to give (Horizon has 80 for the whole console): a large shared
 * file has a chunk of its own, and small ones are put together in chunks of
 * the size the host suggests.
 */
struct chunk {
	uint64_t addr, size;   /* our own mapping of it; size is 0 when the slot is free */
	uint64_t used;         /* pages of it that files have */
	int handle;
	int pooled;            /* shared between files, by chunk_pages[] */
};

/* A file with shared mappings: its contents while it has some. */
struct shared {
	uint64_t dev, ino;
	uint64_t addr, size;   /* where we see all of it, to load and save it */
	struct chunk *chunk;   /* NULL: the host has no shared memory, views are at addr */
	uint64_t off;          /* where the file is in its chunk */
	int views;             /* mappings the guest has of it */
	int kept;              /* without views, but the file may be mapped again */
	int fd;                /* the file, to write the copy back */
	int own;               /* fd is an opening of our own: see is_dead() */
	int written;           /* some view was writable */
};

/* A view maps the whole chunk of its file; the guest has the part it asked
 * for. */
struct view {
	uint64_t base;         /* where the chunk is mapped for this view */
	uint64_t addr, pages;  /* the guest's part; pages is 0 when the slot is free */
	struct shared *object;
};

static struct chunk chunks[MAX_CHUNKS];
static uint8_t chunk_pages[MAX_CHUNKS][CHUNK_MAX_PAGES];
static struct shared shared[MAX_SHARED];
static struct view views[MAX_VIEWS];
static uint8_t *arena;
static uint8_t page_state[ARENA_MAX_PAGES];
static uint64_t arena_size;    /* what the host could reserve */
static uint64_t search_hint;
static struct image images[MAX_IMAGES];
static uint64_t brk_start, brk_cur;
static struct host_lock *lock;

static inline uint64_t page_of(uint64_t addr) { return (addr - (uint64_t)arena) / GX_PAGE; }
static inline int in_arena(uint64_t addr, uint64_t len)
{
	return addr >= (uint64_t)arena && len <= arena_size &&
	       addr - (uint64_t)arena <= arena_size - len;
}

int gmem_init(void)
{
	size_t size = GX_ARENA_SIZE;

	/* As much as asked for if the host has it, less otherwise: programs
	 * that reserve a lot will find out. */
	arena = host_reserve(&size);
	arena_size = GX_PAGE_DOWN(size);
	void *hole;

	if (!arena || arena_size < ARENA_MIN || arena_size > GX_ARENA_SIZE)
		return -1;
	for (int i = 0; host_reserved_hole(i, &hole, &size); i++) {
		uint64_t from = GX_PAGE_DOWN(hole), to = GX_PAGE_UP((uint64_t)hole + size);

		if (in_arena(from, to - from))
			memset(&page_state[page_of(from)], PG_USED | PG_HOLE, (to - from) / GX_PAGE);
	}
	lock = host_lock_new();
	return 0;
}

/* Finds free pages at an address that is a multiple of align bytes. */
static uint64_t find_free_aligned(uint64_t pages, uint64_t align)
{
	for (int pass = 0; pass < 2; pass++) {
		uint64_t addr = (uint64_t)arena + (pass ? 0 : search_hint) * GX_PAGE;

		for (;;) {
			uint64_t first, p;

			addr = (addr + align - 1) & ~(align - 1);
			first = page_of(addr);
			if (first >= (arena_size / GX_PAGE) || pages > (arena_size / GX_PAGE) - first)
				break;
			for (p = first; p < first + pages && !(page_state[p] & PG_USED); p++)
				;
			if (p == first + pages) {
				if (align == GX_PAGE)
					search_hint = p;
				return addr;
			}
			addr = (uint64_t)arena + (p + 1) * GX_PAGE;
		}
	}
	return 0;
}

static uint64_t find_free(uint64_t pages)
{
	return find_free_aligned(pages, GX_PAGE);
}

/* Large anonymous mappings are given the alignment of their size: allocators
 * that need aligned memory (glibc's arenas, PartitionAlloc's pools of tens of
 * gigabytes) otherwise map twice as much to cut an aligned piece out of it,
 * which the arena may not have. */
static uint64_t find_free_anonymous(uint64_t size)
{
	uint64_t align = GX_PAGE;

	if (size >= (2UL << 20))
		while (align * 2 <= size && align < (32UL << 30))
			align *= 2;
	return find_free_aligned(size / GX_PAGE, align);
}

/* What a host that commits for real has to find: pages committed now and at
 * most, and how far into the arena the guest got. */
static uint64_t committed_pages, committed_peak, top_page;
static int chunk_count, chunk_peak;   /* pieces of the host's shared memory */

static void count_committed(int64_t pages, uint64_t last_page)
{
	committed_pages += pages;
	if (committed_pages > committed_peak)
		committed_peak = committed_pages;
	if (last_page > top_page)
		top_page = last_page;
}

void gmem_report(void)
{
	host_log("gx: guest memory: %lu MB committed at most, %lu MB at exit, %lu MB of addresses, "
		 "%d pieces of shared memory at most\n",
		 (unsigned long)(committed_peak >> 8), (unsigned long)(committed_pages >> 8),
		 (unsigned long)(top_page >> 8), chunk_peak);
}

static void release(uint64_t addr, uint64_t len)
{
	uint64_t first = page_of(addr), n = len / GX_PAGE;
	uint64_t committed = 0;

	for (uint64_t p = first; p < first + n; p++)
		committed += !!(page_state[p] & PG_COMMITTED);
	count_committed(-(int64_t)committed, 0);
	if (committed)
		host_decommit((void *)addr, len);
	memset(&page_state[first], 0, n);
	if (first < search_hint)
		search_hint = first;
}

static int commit(uint64_t addr, uint64_t len, int prot)
{
	uint8_t state = PG_USED | (prot & PG_PROT);

	if (prot & PG_PROT) {
		if (host_commit((void *)addr, len, prot))
			return -1;
		state |= PG_COMMITTED;
		count_committed(len / GX_PAGE, page_of(addr) + len / GX_PAGE);
	}
	memset(&page_state[page_of(addr)], state, len / GX_PAGE);
	return 0;
}

static struct image *image_of(uint64_t addr);

/* Whether memory at addr, for a given protection, is data in the sense above:
 * not code, and not part of a file's image (where a page is what its segment
 * says). */
static int is_data(uint64_t addr, int prot)
{
	return !(prot & GX_PROT_EXEC) && !image_of(addr);
}

/* Commits [addr, addr+len), reserved and not committed yet, as data the guest
 * sees with prot. Whole units where a unit is all reservation, so that the
 * pieces a guest commits one after the other are one mapping for the host. */
static int commit_data(uint64_t addr, uint64_t len, int prot)
{
	uint64_t end = addr + len;

	for (uint64_t unit = addr & ~(DATA_UNIT - 1); unit < end; unit += DATA_UNIT) {
		uint64_t from = unit > addr ? unit : addr;
		uint64_t to = unit + DATA_UNIT < end ? unit + DATA_UNIT : end;
		int whole = in_arena(unit, DATA_UNIT);

		for (uint64_t a = unit; whole && a < unit + DATA_UNIT; a += GX_PAGE)
			whole = (page_state[page_of(a)] &
				 (PG_USED | PG_COMMITTED | PG_SHARED | PG_COPY | PG_HOLE)) == PG_USED;
		if (whole && !image_of(unit) && !image_of(unit + DATA_UNIT - 1)) {
			if (host_commit((void *)unit, DATA_UNIT, DATA_PROT))
				return -1;
			/* All of it is there; the guest has what it asked for. */
			memset(&page_state[page_of(unit)], PG_USED | PG_COMMITTED, DATA_UNIT / GX_PAGE);
			count_committed(DATA_UNIT / GX_PAGE, page_of(unit) + DATA_UNIT / GX_PAGE);
			continue;
		}
		/* A unit with something else in it: what is asked, no more, and
		 * only what an earlier unit did not bring. */
		for (uint64_t a = from; a < to;) {
			uint64_t b = a;

			if (page_state[page_of(a)] & PG_COMMITTED) {
				a += GX_PAGE;
				continue;
			}
			while (b < to && !(page_state[page_of(b)] & PG_COMMITTED))
				b += GX_PAGE;
			if (host_commit((void *)a, b - a, DATA_PROT))
				return -1;
			memset(&page_state[page_of(a)], PG_USED | PG_COMMITTED, (b - a) / GX_PAGE);
			count_committed((b - a) / GX_PAGE, page_of(b));
			a = b;
		}
	}
	memset(&page_state[page_of(addr)], PG_USED | PG_COMMITTED | (prot & PG_PROT), len / GX_PAGE);
	return 0;
}

static struct image *image_of(uint64_t addr)
{
	for (int i = 0; i < MAX_IMAGES; i++)
		if (images[i].end && addr >= images[i].start && addr < images[i].end)
			return &images[i];
	return NULL;
}

static uint64_t pool_size_for(uint64_t len)
{
	uint64_t size = GX_PAGE_UP(len / 128);

	return size < 16 * GX_PAGE ? 16 * GX_PAGE : size;
}

/* Records [addr, addr+len) as an image whose veneers live at pool. */
static struct image *add_image(uint64_t addr, uint64_t len, uint64_t pool, uint64_t pool_size)
{
	for (int i = 0; i < MAX_IMAGES; i++) {
		if (images[i].end)
			continue;
		/* Never writable and executable at once, which Horizon forbids:
		 * patch_syscalls() opens the pool for writing while it fills it. */
		if (commit(pool, pool_size, GX_PROT_READ | GX_PROT_EXEC))
			return NULL;
		images[i].start = addr;
		images[i].end = addr + len;
		images[i].pool = images[i].pool_next = pool;
		images[i].pool_end = pool + pool_size;
		return &images[i];
	}
	return NULL;
}

/* Finds len bytes plus a veneer pool, and records them as an image. */
static uint64_t new_image(uint64_t len)
{
	uint64_t pool = pool_size_for(len);
	uint64_t addr = find_free((len + pool) / GX_PAGE);

	if (!addr || !add_image(addr, len, addr + len, pool))
		return 0;
	return addr;
}

/* Makes an image of a file mapped at an address the guest chose. ld.so does
 * that for every library aligned to more than a page: it reserves anonymous
 * memory, then maps the file inside it. The pool goes in the first hole after
 * the mapping. */
static struct image *adopt_image(uint64_t addr, uint64_t len)
{
	uint64_t pool = pool_size_for(len), run = 0;

	for (uint64_t p = page_of(addr + len); p < (arena_size / GX_PAGE); p++) {
		run = (page_state[p] & PG_USED) ? 0 : run + 1;
		if (run == pool / GX_PAGE)
			return add_image(addr, len, (uint64_t)arena + (p + 1) * GX_PAGE - pool, pool);
	}
	return NULL;
}

/* Replaces each svc #0 in [addr, addr+len) by a branch to a veneer. The pool
 * is not executable while veneers are added to it: that is safe as long as an
 * image is mapped entirely before any of it runs, which is what ld.so does. */
static int patch_syscalls(uint64_t addr, uint64_t len)
{
	struct image *img = image_of(addr);
	uint32_t *code = (uint32_t *)addr;
	int patched = 0, ret = 0;

	for (uint64_t k = 0; k < len / 4; k++) {
		uint64_t site = addr + k * 4;
		uint32_t *v;
		int64_t to_veneer, back;

		if (code[k] != SVC0)
			continue;
		if (!img)
			img = adopt_image(addr, len);
		if (!img || img->pool_next + VENEER_SIZE > img->pool_end) {
			host_log("gx: no veneer left for the svc at %#lx\n", (unsigned long)site);
			ret = -1;
			break;
		}
		if (!patched && host_protect((void *)img->pool, img->pool_end - img->pool,
					     GX_PROT_READ | GX_PROT_WRITE)) {
			host_log("gx: cannot open the veneer pool at %#lx\n",
				 (unsigned long)img->pool);
			return -1;
		}
		v = (uint32_t *)img->pool_next;
		to_veneer = (int64_t)(img->pool_next - site);
		back = (int64_t)(site + 4 - (img->pool_next + 5 * 4));
		if (to_veneer >= BRANCH_RANGE || back <= -BRANCH_RANGE) {
			host_log("gx: the svc at %#lx is too far from its veneer pool\n",
				 (unsigned long)site);
			ret = -1;
			break;
		}
		v[0] = 0xA9BF47F0u; /* stp x16, x17, [sp, #-16]! */
		v[1] = 0x10000070u; /* adr x16, exit */
		v[2] = 0x58000091u; /* ldr x17, trampoline */
		v[3] = 0xD61F0220u; /* br  x17 */
		v[4] = 0xA8C147F0u; /* exit: ldp x16, x17, [sp], #16 */
		v[5] = 0x14000000u | (((uint32_t)(back >> 2)) & 0x03FFFFFFu);
		*(uint64_t *)&v[6] = (uint64_t)gx_svc_trampoline;
		code[k] = 0x14000000u | (((uint32_t)(to_veneer >> 2)) & 0x03FFFFFFu);
		img->pool_next += VENEER_SIZE;
		patched++;
	}
	if (patched) {
		if (host_protect((void *)img->pool, img->pool_end - img->pool,
				 GX_PROT_READ | GX_PROT_EXEC)) {
			host_log("gx: cannot close the veneer pool at %#lx\n",
				 (unsigned long)img->pool);
			return -1;
		}
		host_flush_icache((void *)img->pool, img->pool_next - img->pool);
	}
	return ret ? ret : patched;
}

static long read_file(int fd, uint64_t off, uint8_t *dst, uint64_t len)
{
	uint64_t done = 0;

	while (done < len) {
		long n = lkl_sys_pread64(fd, (char *)dst + done, len - done, off + done);

		if (n < 0)
			return n;
		if (n == 0)
			break;
		done += n;
	}
	return 0;
}

/* Takes one mapping of a chunk out of the arena. */
static void unmap_chunk(struct chunk *c, uint64_t base)
{
	host_shared_unmap(c->handle, (void *)base, c->size);
	memset(&page_state[page_of(base)], 0, c->size / GX_PAGE);
	if (page_of(base) < search_hint)
		search_hint = page_of(base);
}

/* Where a mapping of a chunk goes. On a 2 MB boundary when the chunk is that
 * large: the host's kernel can then map it in 2 MB pieces instead of pages,
 * and its page tables are a resource that runs out (on Horizon, a few hundred
 * mappings of 16 MB made of pages take them all, and the next one fails). */
static uint64_t find_chunk_place(uint64_t size)
{
	return find_free_aligned(size / GX_PAGE, size >= (2UL << 20) ? 2UL << 20 : GX_PAGE);
}

/* Gets a chunk of the host's shared memory, mapped for ourselves. */
static struct chunk *new_chunk(uint64_t size, int pooled)
{
	struct chunk *c = NULL;

	for (int i = 0; i < MAX_CHUNKS && !c; i++)
		if (!chunks[i].size)
			c = &chunks[i];
	if (!c)
		return NULL;
	c->addr = find_chunk_place(size);
	c->handle = c->addr ? host_shared_new(size) : -1;
	if (c->handle < 0)
		return NULL;
	if (host_shared_map(c->handle, (void *)c->addr, size, GX_PROT_READ | GX_PROT_WRITE)) {
		host_shared_free(c->handle);
		return NULL;
	}
	memset(&page_state[page_of(c->addr)], PG_USED | PG_COMMITTED | GX_PROT_READ | GX_PROT_WRITE,
	       size / GX_PAGE);
	count_committed(size / GX_PAGE, page_of(c->addr) + size / GX_PAGE);
	if (++chunk_count > chunk_peak)
		chunk_peak = chunk_count;
	c->size = size;
	c->pooled = pooled;
	c->used = 0;
	if (pooled)
		memset(chunk_pages[c - chunks], 0, CHUNK_MAX_PAGES);
	return c;
}

/* Finds the memory of a shared file: a place in a chunk that small files
 * share, or a chunk of its own. Returns 0 if the host has none to give. */
static int alloc_shared(struct shared *s)
{
	uint64_t chunk_size = host_shared_chunk(), pages = s->size / GX_PAGE;
	struct chunk *c;

	if (chunk_size > CHUNK_MAX_PAGES * GX_PAGE)
		chunk_size = CHUNK_MAX_PAGES * GX_PAGE;
	if (!chunk_size || s->size > chunk_size / 4) {
		c = new_chunk(s->size, 0);
		if (!c)
			return 0;
		c->used = pages;
		s->chunk = c;
		s->off = 0;
		return 1;
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < MAX_CHUNKS; i++) {
			uint8_t *used = chunk_pages[i];
			uint64_t run = 0;

			c = &chunks[i];
			if (!c->size || !c->pooled)
				continue;
			for (uint64_t p = 0; p < c->size / GX_PAGE; p++) {
				run = used[p] ? 0 : run + 1;
				if (run < pages)
					continue;
				memset(&used[p + 1 - pages], 1, pages);
				c->used += pages;
				s->chunk = c;
				s->off = (p + 1 - pages) * GX_PAGE;
				/* The last file that was here left its contents. */
				memset((void *)(c->addr + s->off), 0, s->size);
				return 1;
			}
		}
		if (pass || !new_chunk(chunk_size, 1))
			break;
	}
	return 0;
}

static void free_shared(struct shared *s)
{
	struct chunk *c = s->chunk;

	if (c->pooled)
		memset(&chunk_pages[c - chunks][s->off / GX_PAGE], 0, s->size / GX_PAGE);
	c->used -= s->size / GX_PAGE;
	if (c->used)
		return;
	unmap_chunk(c, c->addr);
	count_committed(-(int64_t)(c->size / GX_PAGE), 0);
	host_shared_free(c->handle);
	chunk_count--;
	memset(c, 0, sizeof(*c));
}

static int kept_count;

/* Whether a file is of the kernel's memory filesystem, where a file that has
 * no page is a file that was never written. */
int gmem_in_memory_fs(int fd)
{
	struct lkl_statfs fs;
	long params[6] = { fd, (long)&fs };

	return !lkl_syscall(__lkl__NR_fstatfs, params) && (long)fs.f_type == RAMFS_MAGIC;
}

/* Opens the file behind a guest descriptor for ourselves. */
static int open_again(int fd)
{
	char path[32] = "/proc/self/fd/";
	int at = strlen(path), ours;

	for (int digit = 100000; digit; digit /= 10)
		if (fd >= digit || digit == 1)
			path[at++] = '0' + fd / digit % 10;
	path[at] = 0;
	ours = lkl_sys_open(path, LKL_O_RDWR, 0);
	return ours >= 0 ? ours : lkl_sys_open(path, LKL_O_RDONLY, 0);
}

/* Whether nothing can map the file again: it has no name, and nobody but us
 * has it open, not even in a message on its way between two sockets. The
 * kernel grants a write lease on exactly that condition. */
static int is_dead(const struct shared *s)
{
	long params[6] = { s->fd, LKL_F_SETLEASE, LKL_F_WRLCK };
	struct lkl_stat st;

	return s->own && !lkl_sys_fstat(s->fd, &st) && !st.st_nlink &&
	       !lkl_syscall(__lkl__NR_fcntl, params);
}

/* Fills the copy from the file, without making the kernel read holes: a file
 * of memory that was only sized, as shared memory is before it is mapped, is
 * all zeroes but for the page its last byte made (see ftruncate in gsys.c). */
static void load_shared(const struct shared *s, int fd, const struct lkl_stat *st)
{
	if (gmem_in_memory_fs(fd)) {
		uint64_t last = GX_PAGE_DOWN(st->st_size - 1);
		struct { void *base; uint64_t len; } iov = { (void *)(s->addr + last), st->st_size - last };
		long params[6] = { fd, (long)&iov, 1, last, 0, LKL_RWF_NOWAIT };

		if (!st->st_blocks)
			return;
		/* That page alone, if it is in memory: nothing else is. */
		if ((uint64_t)st->st_blocks * 512 <= GX_PAGE && (uint64_t)st->st_size > GX_PAGE &&
		    lkl_syscall(__lkl__NR_preadv2, params) == (long)iov.len)
			return;
	}
	read_file(fd, 0, (uint8_t *)s->addr, st->st_size);
}

/* Gives up the copy of a shared file: its contents go back to the file. */
static void close_shared(struct shared *s)
{
	struct lkl_stat st;

	if (s->kept)
		kept_count--;

	if (s->fd >= 0) {
		if (s->written && !lkl_sys_fstat(s->fd, &st)) {
			uint64_t len = (uint64_t)st.st_size < s->size ? (uint64_t)st.st_size : s->size;

			for (uint64_t done = 0; done < len;) {
				long n = lkl_sys_pwrite64(s->fd, (char *)s->addr + done, len - done, done);

				if (n <= 0)
					break;
				done += n;
			}
		}
		lkl_sys_close(s->fd);
	}
	if (s->chunk)
		free_shared(s);
	else
		release(s->addr, s->size);
	memset(s, 0, sizeof(*s));
}

/* Forgets a shared file that is dead: nothing of it is worth writing. */
static void discard_shared(struct shared *s)
{
	lkl_sys_close(s->fd);
	kept_count--;
	free_shared(s);
	memset(s, 0, sizeof(*s));
}

/* The files without a view are too many: the dead go, then the oldest in the
 * table until half are left. */
static void collect_shared(void)
{
	for (int i = 0; i < MAX_SHARED; i++)
		if (shared[i].kept && is_dead(&shared[i]))
			discard_shared(&shared[i]);
	for (int i = 0; i < MAX_SHARED && kept_count > KEEP_MAX / 2; i++)
		if (shared[i].kept)
			close_shared(&shared[i]);
}

/* The last view of a shared file is gone. The copy stays for a while: shared
 * memory is often unmapped by who filled it before who reads it maps it, and
 * writing it to the file and reading it back each time is most of the cost
 * of a picture going from one thread of Chromium to another. */
static void retire_shared(struct shared *s)
{
	if (!s->chunk || !s->own) {
		close_shared(s);
		return;
	}
	s->kept = 1;
	if (++kept_count > KEEP_MAX)
		collect_shared();
}

uint8_t *gmem_shared_peek(int fd, uint64_t *size)
{
	struct lkl_stat st;
	uint8_t *found = NULL;

	if (lkl_sys_fstat(fd, &st))
		return NULL;
	host_lock_acquire(lock);
	for (int i = 0; i < MAX_SHARED && !found; i++) {
		if ((shared[i].views || shared[i].kept) && shared[i].dev == st.st_dev &&
		    shared[i].ino == st.st_ino) {
			found = (uint8_t *)shared[i].addr;
			*size = shared[i].size;
		}
	}
	host_lock_release(lock);
	return found;
}

/* How much of the arena a view takes: all of its file's chunk. */
static uint64_t view_span(const struct view *v)
{
	return v->object->chunk ? v->object->chunk->size : v->object->size;
}

/* Unmaps [addr, addr+size), which may hold views of shared files. */
static void unmap_range(uint64_t addr, uint64_t size)
{
	uint64_t end = addr + size;

	while (addr < end) {
		uint8_t kind = page_state[page_of(addr)] & (PG_SHARED | PG_COPY | PG_HOLE);
		uint64_t run = GX_PAGE;
		struct view *v = NULL;

		while (addr + run < end &&
		       (page_state[page_of(addr + run)] & (PG_SHARED | PG_COPY | PG_HOLE)) == kind)
			run += GX_PAGE;
		if (kind & PG_HOLE) {
			addr += run;   /* never the guest's */
			continue;
		}
		if (!kind) {
			release(addr, run);
			addr += run;
			continue;
		}
		for (int i = 0; i < MAX_VIEWS && !v; i++)
			if (views[i].pages && addr >= views[i].base &&
			    addr < views[i].base + view_span(&views[i]))
				v = &views[i];
		if (!v) {
			addr += GX_PAGE;   /* not the guest's to unmap */
			continue;
		}
		if (run > v->base + view_span(v) - addr)
			run = v->base + view_span(v) - addr;
		/* The mapping goes when the guest has unmapped as much as it
		 * was given, in however many pieces. */
		v->pages -= run / GX_PAGE < v->pages ? run / GX_PAGE : v->pages;
		if (!v->pages) {
			struct shared *s = v->object;

			if (kind == PG_SHARED)
				unmap_chunk(s->chunk, v->base);
			if (!--s->views)
				retire_shared(s);
		}
		addr += run;
	}
}

/* A shared mapping of a regular file. Returns 0 if this one cannot be shared
 * (not a regular file, a file that outgrew its copy...) and is to be mapped
 * as a private copy instead. */
static long map_shared(uint64_t len, int prot, int fd, uint64_t off)
{
	uint64_t size = GX_PAGE_UP(len), base = 0;
	struct shared *s = NULL, *slot = NULL;
	struct view *v = NULL;
	struct lkl_stat st;

	if (lkl_sys_fstat(fd, &st) || (st.st_mode & LKL_S_IFMT) != LKL_S_IFREG)
		return 0;
	for (int i = 0; i < MAX_VIEWS && !v; i++)
		if (!views[i].pages)
			v = &views[i];
	for (int i = 0; i < MAX_SHARED; i++) {
		if (!shared[i].views && !shared[i].kept) {
			if (!slot)
				slot = &shared[i];
		} else if (shared[i].dev == st.st_dev && shared[i].ino == st.st_ino) {
			s = &shared[i];
		}
	}
	if (s && s->kept && off + size > s->size) {
		/* It grew since: start again from the file. */
		close_shared(s);
		slot = s;
		s = NULL;
	}
	if (!v || (!s && !slot))
		return 0;

	if (!s) {
		s = slot;
		memset(s, 0, sizeof(*s));
		s->size = GX_PAGE_UP((uint64_t)st.st_size > off + len ? (uint64_t)st.st_size : off + len);
		if (alloc_shared(s)) {
			s->addr = s->chunk->addr + s->off;
		} else {
			/* No such memory on this host, or no more of it: one
			 * plain copy, which every view will be. Programs that
			 * tell their mappings apart by address (Chromium) do
			 * not survive it for long. */
			static int warned;

			if (!warned++)
				host_log("gx: no shared memory for a file of %lu KB: its mappings "
					 "will all be at one address\n", (unsigned long)(s->size >> 10));
			s->chunk = NULL;
			s->addr = find_free(s->size / GX_PAGE);
			if (!s->addr || commit(s->addr, s->size, GX_PROT_READ | GX_PROT_WRITE))
				return -GX_ENOMEM;
			memset(&page_state[page_of(s->addr)],
			       PG_USED | PG_COMMITTED | PG_COPY | GX_PROT_READ | GX_PROT_WRITE,
			       s->size / GX_PAGE);
		}
		load_shared(s, fd, &st);
		s->dev = st.st_dev;
		s->ino = st.st_ino;
		s->fd = open_again(fd);
		s->own = s->fd >= 0;
		if (!s->own)
			s->fd = lkl_sys_dup(fd);
		s->views = 1;   /* held until this view exists or fails */
	} else {
		if (s->kept) {
			s->kept = 0;
			kept_count--;
		}
		s->views++;
	}

	if (off + size > s->size) {
		host_log("gx: a shared file grew under its mappings; mapping a copy\n");
	} else if (!s->chunk) {
		base = s->addr;
		v->addr = base + off;
	} else {
		base = find_chunk_place(s->chunk->size);
		if (base && host_shared_map(s->chunk->handle, (void *)base, s->chunk->size, prot))
			base = 0;
		if (base)
			memset(&page_state[page_of(base)],
			       PG_USED | PG_COMMITTED | PG_SHARED | (prot & PG_PROT),
			       s->chunk->size / GX_PAGE);
		v->addr = base + s->off + off;
	}
	if (!base) {
		if (!--s->views)
			close_shared(s);
		return 0;
	}
	v->base = base;
	v->pages = size / GX_PAGE;
	v->object = s;
	s->written |= prot & GX_PROT_WRITE;
	return (long)v->addr;
}

static long do_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
	uint64_t size = GX_PAGE_UP(len);
	int anonymous = (flags & GX_MAP_ANONYMOUS) || fd < 0;
	long ret;

	if (!len)
		return -GX_EINVAL;

	if (!anonymous && (flags & GX_MAP_SHARED) &&
	    !(flags & (GX_MAP_FIXED | GX_MAP_FIXED_NOREPLACE)) && !(prot & GX_PROT_EXEC) &&
	    !(off & (GX_PAGE - 1))) {
		ret = map_shared(len, prot, fd, off);
		if (ret)
			return ret;
	}

	if (flags & (GX_MAP_FIXED | GX_MAP_FIXED_NOREPLACE)) {
		if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size))
			return -GX_ENOMEM;
		for (uint64_t p = page_of(addr); p < page_of(addr) + size / GX_PAGE; p++)
			if (page_state[p] & PG_HOLE)
				return -GX_ENOMEM;
		if (flags & GX_MAP_FIXED_NOREPLACE) {
			for (uint64_t p = page_of(addr); p < page_of(addr) + size / GX_PAGE; p++)
				if (page_state[p] & PG_USED)
					return -17; /* EEXIST */
		}
		unmap_range(addr, size);
	} else {
		addr = anonymous ? find_free_anonymous(size) : new_image(size);
		if (!addr)
			return -GX_ENOMEM;
	}

	if (anonymous) {
		/* Data is the host's to read and write whatever the guest says. */
		if ((prot & PG_PROT) && is_data(addr, prot)) {
			if (host_commit((void *)addr, size, DATA_PROT))
				return -GX_ENOMEM;
			count_committed(size / GX_PAGE, page_of(addr) + size / GX_PAGE);
			memset(&page_state[page_of(addr)], PG_USED | PG_COMMITTED | (prot & PG_PROT),
			       size / GX_PAGE);
			return (long)addr;
		}
		return commit(addr, size, prot) ? -GX_ENOMEM : (long)addr;
	}

	if (commit(addr, size, GX_PROT_READ | GX_PROT_WRITE))
		return -GX_ENOMEM;
	ret = read_file(fd, off, (uint8_t *)addr, len);
	if (ret < 0) {
		release(addr, size);
		return ret;
	}
	if (prot & GX_PROT_EXEC) {
		if (patch_syscalls(addr, size) < 0) {
			release(addr, size);
			return -GX_ENOMEM;
		}
		host_flush_icache((void *)addr, size);
	}
	if (prot != (GX_PROT_READ | GX_PROT_WRITE)) {
		if (prot & PG_PROT) {
			if (host_protect((void *)addr, size, prot)) {
				release(addr, size);
				return -GX_ENOMEM;
			}
			memset(&page_state[page_of(addr)],
			       PG_USED | PG_COMMITTED | (prot & PG_PROT), size / GX_PAGE);
		} else {
			release(addr, size);
			commit(addr, size, 0);
		}
	}
	return (long)addr;
}

long gmem_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
	long ret;

	host_lock_acquire(lock);
	ret = do_mmap(addr, len, prot, flags, fd, off);
	host_lock_release(lock);
	return ret;
}

long gmem_reserve_image(uint64_t span)
{
	uint64_t addr;

	host_lock_acquire(lock);
	addr = new_image(GX_PAGE_UP(span));
	if (addr)
		commit(addr, GX_PAGE_UP(span), 0);
	host_lock_release(lock);
	return addr ? (long)addr : -GX_ENOMEM;
}

long gmem_munmap(uint64_t addr, uint64_t len)
{
	uint64_t size = GX_PAGE_UP(len);

	if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size))
		return -GX_EINVAL;
	host_lock_acquire(lock);
	unmap_range(addr, size);
	for (int i = 0; i < MAX_IMAGES; i++) {
		struct image *img = &images[i];

		if (!img->end || addr > img->start || addr + size < img->end)
			continue;
		release(img->pool, img->pool_end - img->pool);
		memset(img, 0, sizeof(*img));
	}
	host_lock_release(lock);
	return 0;
}

long gmem_mprotect(uint64_t addr, uint64_t len, int prot)
{
	uint64_t size = GX_PAGE_UP(len);
	uint64_t first, n;
	long ret = 0;

	if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size))
		return -GX_ENOMEM;
	host_lock_acquire(lock);
	first = page_of(addr);
	n = size / GX_PAGE;
	/* Pages are handled in runs that share a state, so that a run mapped
	 * PROT_NONE is committed and the others only change protection. */
	for (uint64_t p = first; p < first + n && !ret;) {
		uint64_t q = p;
		uint64_t run_addr = (uint64_t)arena + p * GX_PAGE;

		while (q < first + n && page_state[q] == page_state[p])
			q++;
		if (!(page_state[p] & PG_USED) || (page_state[p] & PG_HOLE)) {
			ret = -GX_ENOMEM;
		} else if (page_state[p] & PG_COPY) {
			/* One protection cannot suit every view of the copy. */
		} else if (!(page_state[p] & PG_COMMITTED)) {
			if (!(prot & PG_PROT))
				;   /* reserved it was, reserved it stays */
			else if (is_data(run_addr, prot) ? commit_data(run_addr, (q - p) * GX_PAGE, prot)
							 : commit(run_addr, (q - p) * GX_PAGE, prot))
				ret = -GX_ENOMEM;
		} else if (!(page_state[p] & (PG_SHARED | GX_PROT_EXEC)) && is_data(run_addr, prot)) {
			/* Data: the protection is the guest's idea of it, no more. */
			memset(&page_state[p], PG_USED | PG_COMMITTED | (prot & PG_PROT), q - p);
		} else {
			/* Dropping all access keeps the contents: the guest may restore it. */
			if (host_protect((void *)run_addr, (q - p) * GX_PAGE, prot)) {
				/* A view of a shared file may be stuck with the
				 * protection it was mapped with. */
				if (!(page_state[p] & PG_SHARED))
					ret = -13; /* EACCES: the host refuses this protection */
			} else
				memset(&page_state[p], PG_USED | PG_COMMITTED | (prot & PG_PROT) |
				       (page_state[p] & PG_SHARED), q - p);
		}
		p = q;
	}
	host_lock_release(lock);
	return ret;
}

long gmem_madvise(uint64_t addr, uint64_t len, int advice)
{
	uint64_t size = GX_PAGE_UP(len);

	if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size))
		return -GX_EINVAL;
	if (advice != MADV_DONTNEED)
		return 0;
	/* Private anonymous memory reads as zeroes afterwards, and allocators
	 * count on it. */
	host_lock_acquire(lock);
	for (uint64_t p = page_of(addr), end = p + size / GX_PAGE; p < end;) {
		uint8_t state = page_state[p];
		uint64_t q = p;

		while (q < end && page_state[q] == state)
			q++;
		if (!(state & PG_COMMITTED) || (state & (PG_SHARED | PG_COPY))) {
			/* nothing there, or not private */
		} else if ((state & GX_PROT_WRITE) ||
			   (!(state & PG_PROT) && is_data((uint64_t)arena + p * GX_PAGE, 0))) {
			/* Cleared and kept, also what has no access (how
			 * PartitionAlloc decommits): giving it back would cut the
			 * host's mapping in pieces. */
			memset(arena + p * GX_PAGE, 0, (q - p) * GX_PAGE);
		} else if (!(state & PG_PROT)) {
			host_decommit(arena + p * GX_PAGE, (q - p) * GX_PAGE);
			memset(&page_state[p], PG_USED, q - p);
			count_committed(-(int64_t)(q - p), 0);
		}
		p = q;
	}
	host_lock_release(lock);
	return 0;
}

int gmem_accessible(uint64_t addr, uint64_t len, int write)
{
	uint8_t need = PG_COMMITTED | (write ? GX_PROT_WRITE : GX_PROT_READ);

	if (!len || !in_arena(addr, 1))
		return 1;   /* not ours to judge: the host's own memory */
	if (!in_arena(addr, len))
		return 0;
	for (uint64_t p = page_of(addr); p <= page_of(addr + len - 1); p++)
		if ((page_state[p] & need) != need)
			return 0;
	return 1;
}

void gmem_set_brk(uint64_t start)
{
	(void)start;
	host_lock_acquire(lock);
	/* The break lives in its own reservation; nothing needs it to follow
	 * the program image. */
	brk_start = find_free(BRK_SIZE / GX_PAGE);
	if (brk_start)
		memset(&page_state[page_of(brk_start)], PG_USED, BRK_SIZE / GX_PAGE);
	brk_cur = brk_start;
	host_lock_release(lock);
}

long gmem_brk(uint64_t addr)
{
	host_lock_acquire(lock);
	if (brk_start && addr >= brk_start && addr <= brk_start + BRK_SIZE) {
		uint64_t old_top = GX_PAGE_UP(brk_cur), new_top = GX_PAGE_UP(addr);

		if (new_top > old_top) {
			if (commit(old_top, new_top - old_top, GX_PROT_READ | GX_PROT_WRITE) == 0)
				brk_cur = addr;
		} else {
			if (new_top < old_top) {
				host_decommit((void *)new_top, old_top - new_top);
				memset(&page_state[page_of(new_top)], PG_USED,
				       (old_top - new_top) / GX_PAGE);
			}
			brk_cur = addr;
		}
	}
	addr = brk_cur;
	host_lock_release(lock);
	return (long)addr;
}

/* For crash reports: the image addr belongs to, if any. */
int gmem_describe(uint64_t addr, uint64_t *base)
{
	struct image *img;

	if (!in_arena(addr, 1))
		return 0;
	img = image_of(addr);
	*base = img ? img->start : (uint64_t)arena;
	return img ? 2 : 1;
}
