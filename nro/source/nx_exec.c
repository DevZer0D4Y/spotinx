/*
 * nx_exec.c - fase 8: executor de binarios ELF aarch64 no Switch.
 *
 * Porta a cadeia COMPLETA validada no PC (pc-test/exec-hw.c) para o Horizon:
 * roda um busybox aarch64 static-PIE de verdade sobre o kernel LKL.
 *
 * Diferencas para o PC:
 *  - Memoria executavel: svcMapProcessCodeMemory + permissoes por segmento.
 *  - brk/mmap emulados: memoria da heap do libnx (memalign), acessivel pelo LKL.
 *  - console fds 0/1/2: nx_print (tela) no lugar de write() do host.
 *  - auxv HWCAP: valor fixo (sem getauxval no Horizon).
 *  - Alvo static-PIE (nao EXEC 0x400000): jit da base arbitraria, PIE roda nela.
 *
 * Pecas (todas provadas no PC): load PT_LOAD, R_AARCH64_RELATIVE, veneer
 * entry+exit por-svc (preserva x16/x17), trampolim com preservacao completa
 * x0-x18/x30/NZCV/FP/TP + stack dedicado + q0-q31, stack de processo (argc/argv/
 * envp/auxv, sp alinhado a 16, x0=0 na entrada), reserva de fds 0/1/2, traducao
 * de flags O_* (aarch64 -> lkl/asm-generic).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <switch.h>
#include <lkl.h>
#include <lkl_host.h>

#define SVC0_INSN   0xD4000001u
#define VENEER_POOL 0x8000  /* 32KB p/ veneers (32B cada; busybox tem ~300 svc) */
#define R_AARCH64_RELATIVE 1027
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define DT_RELA 7
#define DT_RELASZ 8

extern void nx_svc_trampoline(void);   /* nx_trampoline.s */
extern volatile int nx_display_pause;  /* main.c: pausa console durante CodeMemory */
long g_host_tp;                        /* TP do host, capturado antes do guest */
long g_host_stack;                     /* topo do stack dedicado do host/LKL */

/* ---------- brk/mmap emulados (heap do libnx) ---------- */

#define GUEST_HEAP_SIZE (24 * 1024 * 1024)
static uint8_t *g_heap_base, *g_heap_cur, *g_heap_end;

static long emulate_brk(long addr)
{
	if (!g_heap_base) {
		g_heap_base = memalign(0x1000, GUEST_HEAP_SIZE);
		/* brk/sbrk do Linux expoe paginas novas zeradas. A heap do libnx pode
		 * conter dados antigos; sem isto o ptmalloc interpreta lixo como estado. */
		if (!g_heap_base)
			return 0;
		memset(g_heap_base, 0, GUEST_HEAP_SIZE);
		g_heap_cur = g_heap_base;
		g_heap_end = g_heap_base + GUEST_HEAP_SIZE;
	}
	if (addr == 0)
		return (long)g_heap_cur;
	if ((uint8_t *)addr < g_heap_base || (uint8_t *)addr > g_heap_end)
		return (long)g_heap_cur;
	g_heap_cur = (uint8_t *)addr;
	return (long)g_heap_cur;
}

/* ---------- ponte de syscall (chamada pelo trampolim) ---------- */

extern void nx_console_write(const char *s, int len);  /* main.c: escreve na tela */

/*
 * Layout do frame salvo pelo trampolim: [0..7]=x0..x7, [8]=x8(nr). args=regs[0..5].
 */
long nx_do_syscall(long *regs)
{
	long nr = regs[8];
	long params[6] = { regs[0], regs[1], regs[2], regs[3], regs[4], regs[5] };

	/* console fds 0/1/2 */
	if (nr == 64 /*write*/ && (regs[0] == 1 || regs[0] == 2)) {
		nx_console_write((const char *)regs[1], (int)regs[2]);
		return regs[2];
	}
	if (nr == 79 /*newfstatat*/ &&
	    (regs[0] == 0 || regs[0] == 1 || regs[0] == 2)) {
		/* finge um character device (tty). struct stat: st_mode em +16.
		 * S_IFCHR=0020000. Zeramos o buf e setamos o modo. */
		if (regs[2]) {
			memset((void *)regs[2], 0, 128);
			*(unsigned int *)(regs[2] + 16) = 0020000 | 0666;
		}
		return 0;
	}
	if (nr == 29 /*ioctl*/ &&
	    (regs[0] == 0 || regs[0] == 1 || regs[0] == 2))
		return 0;

	if (nr == 214) { /* brk */
		long ret = emulate_brk(regs[0]);
		FILE *lf = fopen("sdmc:/lkl-stage.log", "a");
		if (lf) {
			fprintf(lf, "Y: brk(0x%lx)=0x%lx\n", regs[0], ret);
			fclose(lf);
		}
		return ret;
	}

	switch (nr) {
	case 226: /* mprotect */
	case 99:  /* set_robust_list */
	case 293: /* rseq */
	case 135: /* rt_sigprocmask */
		return 0;
	case 222: { /* mmap anonimo -> heap do libnx */
		long len = regs[1], flags = regs[3], gfd = regs[4];

		if (gfd == -1 || (flags & 0x20)) {
			size_t alloc_len = (len + 0xFFF) & ~0xFFFUL;
			void *m = memalign(0x1000, alloc_len);
			/* MAP_ANONYMOUS garante memoria inicializada com zero. Isto inclui
			 * o bloco TLS/TCB da glibc; lixo aqui corrompe variaveis __thread. */
			if (m)
				memset(m, 0, alloc_len);
			FILE *lf = fopen("sdmc:/lkl-stage.log", "a");
			if (lf) {
				fprintf(lf, "Y: mmap(len=0x%lx)=0x%lx\n",
					len, m ? (long)m : -12L);
				fclose(lf);
			}

			return m ? (long)m : -12;
		}
		break;
	}
	}

	/* traducao de flags O_* do openat (aarch64 -> lkl/asm-generic) */
	if (nr == 56) {
		long af = params[2], lf = 0;
		if (af & 03)       lf |= (af & 03);
		if (af & 0100)     lf |= 0100;      /* O_CREAT */
		if (af & 0200)     lf |= 0200;      /* O_EXCL */
		if (af & 01000)    lf |= 01000;     /* O_TRUNC */
		if (af & 02000)    lf |= 02000;     /* O_APPEND */
		if (af & 0x800)    lf |= 04000;     /* O_NONBLOCK */
		if (af & 0x4000)   lf |= 0200000;   /* O_DIRECTORY */
		if (af & 0x8000)   lf |= 0400000;   /* O_NOFOLLOW */
		if (af & 0x80000)  lf |= 02000000;  /* O_CLOEXEC */
		if (af & 0x40000)  lf |= 040000;    /* O_DIRECT */
		params[2] = lf;
	}

	return lkl_syscall(nr, params);
}

/* ---------- ELF ---------- */

struct e64_ehdr {
	unsigned char e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct e64_phdr {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct e64_dyn { int64_t tag; uint64_t val; };
struct e64_rela { uint64_t off, info, addend; };

/* R_AARCH64_RELATIVE: *(base+off) = base + addend (base = rx real de execucao) */
static void apply_relocs(uint8_t *rw, uint64_t base_rx, uint64_t lo,
			 struct e64_phdr *p, int phnum)
{
	for (int i = 0; i < phnum; i++) {
		if (p[i].p_type != PT_DYNAMIC)
			continue;
		struct e64_dyn *d = (struct e64_dyn *)(rw + (p[i].p_vaddr - lo));
		uint64_t rela_off = 0, rela_sz = 0;

		for (; d->tag != 0; d++) {
			if (d->tag == DT_RELA) rela_off = d->val;
			else if (d->tag == DT_RELASZ) rela_sz = d->val;
		}
		if (!rela_off)
			continue;
		struct e64_rela *r = (struct e64_rela *)(rw + (rela_off - lo));
		int n = rela_sz / sizeof(struct e64_rela);

		for (int k = 0; k < n; k++) {
			if ((r[k].info & 0xffffffff) != R_AARCH64_RELATIVE)
				continue;
			/* slot escrito via RW; valor aponta pro espaco RX de execucao */
			uint64_t *slot = (uint64_t *)(rw + (r[k].off - lo));
			*slot = base_rx + (r[k].addend - lo);
		}
	}
}

/* carrega, patcha e executa um ELF static-PIE. Retorna 0 ok. */
int nx_exec_elf(const char *path)
{
	FILE *f = fopen(path, "rb");
	long fsz;
	uint8_t *file;
	struct e64_ehdr *eh;
	struct e64_phdr *ph;
	uint64_t lo = ~0ULL, hi = 0;
	int i, npatch = 0, vi = 0;

	if (!f) { printf("nx_exec: nao abriu %s\n", path); return -1; }
	fseek(f, 0, SEEK_END); fsz = ftell(f); fseek(f, 0, SEEK_SET);
	file = malloc(fsz);
	if (!file || fread(file, 1, fsz, f) != (size_t)fsz) {
		printf("nx_exec: falha na leitura\n"); fclose(f); free(file);
		return -1;
	}
	fclose(f);

	eh = (struct e64_ehdr *)file;
	if (memcmp(eh->e_ident, "\x7f""ELF", 4) || eh->e_ident[4] != 2 ||
	    eh->e_machine != 183) {
		printf("nx_exec: nao e ELF64 aarch64\n"); free(file); return -1;
	}
	ph = (struct e64_phdr *)(file + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD) continue;
		if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
		if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
	}
	if (hi <= lo) { printf("nx_exec: sem PT_LOAD\n"); free(file); return -1; }

	size_t span = (hi - lo + VENEER_POOL + 0xFFF) & ~0xFFFUL;
	uint8_t *rw = memalign(0x1000, span);
	if (!rw) {
		printf("nx_exec: sem memoria-fonte (%lu bytes)\n",
		       (unsigned long)span);
		free(file);
		return -1;
	}

	/*
	 * A imagem nasce em memoria normal RW. O destino final precisa ser conhecido
	 * antes das relocacoes e dos veneers, pois todos os valores/branches devem
	 * usar o load bias do alias executavel definitivo.
	 */
	void *map_addr;
	VirtmemReservation *reservation;
	virtmemLock();
	map_addr = virtmemFindCodeMemory(span, 0);
	reservation = map_addr ? virtmemAddReservation(map_addr, span) : NULL;
	virtmemUnlock();
	if (!map_addr || !reservation) {
		printf("nx_exec: sem faixa de CodeMemory\n");
		free(rw);
		free(file);
		return -1;
	}
	uint8_t *rx = (uint8_t *)map_addr;

	memset(rw, 0, span);
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD) continue;
		memcpy(rw + (ph[i].p_vaddr - lo), file + ph[i].p_offset, ph[i].p_filesz);
	}

	/* PIE: relocacoes apontam pro espaco RX de execucao */
	apply_relocs(rw, (uint64_t)rx, lo, ph, eh->e_phnum);

	/* veneer entry+exit por-svc (identico ao PC, enderecos no espaco RX) */
	uint32_t *code_rw = (uint32_t *)rw;
	uint32_t *vpool_rw = (uint32_t *)(rw + span - VENEER_POOL);
	uintptr_t vpool_rx = (uintptr_t)(rx + span - VENEER_POOL);

	for (size_t k = 0; k < (span - VENEER_POOL) / 4; k++) {
		if (code_rw[k] != SVC0_INSN) continue;
		uint32_t *v = &vpool_rw[vi * 8];
		uintptr_t v_rx = vpool_rx + vi * 32;
		uintptr_t site_rx = (uintptr_t)(rx + k * 4);
		uintptr_t ret_rx = site_rx + 4;
		uintptr_t w5_rx = v_rx + 5 * 4;
		intptr_t boff = (intptr_t)(ret_rx - w5_rx);

		v[0] = 0xA9BF47F0u; /* stp x16,x17,[sp,#-16]! */
		v[1] = 0x10000070u; /* adr x16, w4 (+12) */
		v[2] = 0x58000091u; /* ldr x17, w6 (+16) */
		v[3] = 0xD61F0220u; /* br  x17 */
		v[4] = 0xA8C147F0u; /* ldp x16,x17,[sp],#16 (saida) */
		v[5] = 0x14000000u | (((int32_t)(boff >> 2)) & 0x03FFFFFFu); /* b site+4 */
		*(uint64_t *)&v[6] = (uint64_t)nx_svc_trampoline;

		intptr_t off = (intptr_t)(v_rx - site_rx);
		code_rw[k] = 0x14000000u | (((int32_t)(off >> 2)) & 0x03FFFFFFu); /* b veneer */
		npatch++; vi++;
	}
	printf("nx_exec: %d svc patchados\n", npatch);

	/* Publica a imagem pronta no alias de CodeMemory definitivo. */
	armDCacheClean(rw, span);
	Handle proc = envGetOwnProcessHandle();
	/* nwindowDequeueBuffer pode abortar se a thread de display concorrer com a
	 * transicao do mapa de codigo. Pausamos por duas janelas de frame para deixar
	 * qualquer consoleUpdate ja iniciado terminar. */
	nx_display_pause = 1;
	svcSleepThread(40000000ULL);
	virtmemLock();
	Result rc = svcMapProcessCodeMemory(proc, (u64)rx, (u64)rw, span);
	virtmemRemoveReservation(reservation);
	virtmemUnlock();
	printf("nx_exec: map code src=%p dst=%p span=0x%lx rc=0x%x\n",
	       rw, rx, (unsigned long)span, rc);
	{
		FILE *lf = fopen("sdmc:/lkl-stage.log", "a");
		if (lf) {
			fprintf(lf, "X: map code dst=%p span=0x%lx rc=0x%x\n",
				rx, (unsigned long)span, rc);
			fclose(lf);
		}
	}
	if (R_FAILED(rc)) {
		printf("nx_exec: svcMapProcessCodeMemory falhou\n");
		nx_display_pause = 0;
		free(rw); /* a fonte continua acessivel se o mapeamento falhou */
		free(file);
		return -1;
	}

	/*
	 * W^X do Horizon: cada pagina do alias final recebe uma unica permissao.
	 * A faixa entre PT_LOADs fica somente-leitura; PT_LOADs recebem as flags ELF;
	 * o pool de veneers, que fica depois da imagem, recebe RX.
	 */
	rc = svcSetProcessMemoryPermission(proc, (u64)rx, span, Perm_R);
	printf("nx_exec: span R perm rc=0x%x\n", rc);
	{
		FILE *lf = fopen("sdmc:/lkl-stage.log", "a");
		if (lf) { fprintf(lf, "X: span R perm rc=0x%x\n", rc); fclose(lf); }
	}
	if (R_FAILED(rc)) {
		nx_display_pause = 0;
		free(file);
		return -1;
	}
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		uintptr_t seg_lo = (ph[i].p_vaddr - lo) & ~0xFFFUL;
		uintptr_t seg_hi = (ph[i].p_vaddr - lo + ph[i].p_memsz + 0xFFF) & ~0xFFFUL;
		u32 perm = (ph[i].p_flags & 2 /*PF_W*/) ? Perm_Rw :
			   (ph[i].p_flags & 1 /*PF_X*/) ? Perm_Rx : Perm_R;
		rc = svcSetProcessMemoryPermission(proc, (u64)(rx + seg_lo),
			seg_hi - seg_lo, perm);
		printf("nx_exec: seg [0x%lx..0x%lx] perm=%u rc=0x%x\n",
		       (unsigned long)seg_lo, (unsigned long)seg_hi, perm, rc);
		FILE *lf = fopen("sdmc:/lkl-stage.log", "a");
		if (lf) {
			fprintf(lf, "X: seg [0x%lx..0x%lx] perm=%u rc=0x%x\n",
				(unsigned long)seg_lo, (unsigned long)seg_hi, perm, rc);
			fclose(lf);
		}
		if (R_FAILED(rc)) { nx_display_pause = 0; free(file); return -1; }
	}
	uintptr_t veneer_lo = span - VENEER_POOL;
	rc = svcSetProcessMemoryPermission(proc, (u64)(rx + veneer_lo),
		VENEER_POOL, Perm_Rx);
	printf("nx_exec: veneers [0x%lx..0x%lx] RX rc=0x%x\n",
	       (unsigned long)veneer_lo, (unsigned long)span, rc);
	{
		FILE *lf = fopen("sdmc:/lkl-stage.log", "a");
		if (lf) {
			fprintf(lf, "X: veneers [0x%lx..0x%lx] RX rc=0x%x\n",
				(unsigned long)veneer_lo, (unsigned long)span, rc);
			fclose(lf);
		}
	}
	if (R_FAILED(rc)) { nx_display_pause = 0; free(file); return -1; }
	armICacheInvalidate(rx, span);
	nx_display_pause = 0;

	uintptr_t entry = (uintptr_t)(rx + (eh->e_entry - lo));

	/* --- stack de processo Linux --- */
	static uint64_t gstack[8192];             /* 64KB */
	static uint64_t rnd[2] = { 0x0123456789abcdefULL, 0xfedcba9876543210ULL };
	uint64_t phdr_va = (uint64_t)rx + eh->e_phoff;

	/* argv do guest: por ora, so o nome do binario (argv0). Args fixos via cmd. */
	extern int nx_exec_argc;
	extern char **nx_exec_argv;
	int gargc = nx_exec_argc > 0 ? nx_exec_argc : 1;
	uint64_t *sp = &gstack[4096];

	#define AUX(t, val) do { *(--sp) = (val); *(--sp) = (t); } while (0)
	AUX(0, 0);                    /* AT_NULL */
	AUX(23, 0);                   /* AT_SECURE */
	AUX(25, (uint64_t)rnd);       /* AT_RANDOM */
	AUX(16, 0);                   /* AT_HWCAP (0 = seguro no Horizon) */
	AUX(17, 100);                 /* AT_CLKTCK */
	AUX(6, 4096);                 /* AT_PAGESZ */
	AUX(11, 0); AUX(12, 0); AUX(13, 0); AUX(14, 0); /* UID/EUID/GID/EGID */
	AUX(8, 0);                    /* AT_FLAGS */
	AUX(7, 0);                    /* AT_BASE */
	AUX(9, entry);                /* AT_ENTRY */
	AUX(5, eh->e_phnum);          /* AT_PHNUM */
	AUX(4, eh->e_phentsize);      /* AT_PHENT */
	AUX(3, phdr_va);              /* AT_PHDR */
	#undef AUX
	*(--sp) = 0;                                  /* envp NULL */
	uint64_t *argv_slots = sp - (gargc + 1);
	argv_slots[gargc] = 0;
	for (int a = 0; a < gargc; a++)
		argv_slots[a] = (uint64_t)nx_exec_argv[a];
	sp = argv_slots;
	*(--sp) = gargc;                              /* argc */

	if ((uintptr_t)sp & 8) {                      /* alinha sp a 16 */
		uint64_t *nsp = sp - 1;
		int words = (&gstack[4096]) - sp;
		for (int w = 0; w < words; w++)
			nsp[w] = sp[w];
		sp = nsp;
	}

	/* stack dedicado grande pro C do host/LKL (o kernel LKL usa muito stack) */
	static uint8_t hoststk[8 * 1024 * 1024] __attribute__((aligned(16)));
	g_host_stack = (long)(hoststk + sizeof(hoststk) - 16);

	free(file);
	printf("nx_exec: saltando no guest (entry=0x%lx, %d args)...\n",
	       (unsigned long)entry, gargc);

	/* captura o TP do host; o trampolim restaura em cada syscall */
	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(g_host_tp));

	/* entrada de PROCESSO: x0=0 (senao glibc trata como rtld_fini), br (nao blr) */
	register uint64_t x_sp __asm__("x9") = (uint64_t)sp;
	register uint64_t x_en __asm__("x10") = entry;
	__asm__ volatile(
		"mov sp, %0\n"
		"mov x0, xzr\n"
		"br  %1\n"
		: : "r"(x_sp), "r"(x_en)
		: "x0", "memory");
	__builtin_unreachable();
	return 0;
}
