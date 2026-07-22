/*
 * exec-hw.c - valida no PC a cadeia REAL da fase 7 com um ELF guest de verdade:
 * load -> memoria executavel -> patch svc->bl -> salta no entry -> trampolim ->
 * lkl_syscall. Igual ao nx_exec.c do Switch, trocando jitCreate por mmap(PROT_EXEC)
 * e o trampolim asm por um equivalente. Prova a cadeia antes de ir pro hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <unistd.h>
#include <lkl.h>
#include <lkl_host.h>

#define SVC0 0xD4000001u
#define VENEER_POOL 0x4000  /* 16KB no fim do img p/ veneers (32B cada) */

/*
 * trampolim: preserva TODO x0..x30 do guest, empacota os regs de syscall, chama
 * lkl_syscall, poe retorno em x0 e volta via x17 (endereco fixo apos o svc, que
 * o veneer carregou). NAO usa x30 (o svc foi trocado por `b veneer`, que nao
 * grava x30 — logo o x30 do guest fica intacto para o `ret` do helper sys()).
 *
 * Entrada (do veneer): x17 = endereco de retorno (apos o svc no guest).
 * x8=nr, x0..x5=args ja carregados pelo guest.
 */
/*
 * TROCA DE TPIDR_EL0 (thread pointer) — a peca que faltava.
 * O glibc do guest faz `msr tpidr_el0, guest_tcb` no __libc_setup_tls. Dai em
 * diante, TPIDR_EL0 aponta pro TCB do GUEST. Mas o C do host (lkl_syscall, e ate
 * o nosso fprintf de debug) espera TPIDR_EL0 no TCB do HOST (errno, locks do
 * stdio, stack-protector). Sem trocar, o C do host le TLS do guest -> corrupcao,
 * exatamente apos a instalacao do TLS. O trampolim agora: salva guest TP, poe
 * host TP, chama C, restaura guest TP.
 */
long g_host_tp;   /* capturado antes de saltar pro guest */

__asm__(
	".text\n"
	".global pc_trampoline\n"
	".balign 4\n"
	"pc_trampoline:\n"
	"  sub sp, sp, #160\n"
	"  stp x0, x1, [sp, #0]\n"
	"  stp x2, x3, [sp, #16]\n"
	"  stp x4, x5, [sp, #32]\n"
	"  str x8, [sp, #48]\n"
	"  stp x17, x30, [sp, #56]\n"  /* x17=retorno guest, x30=preserva o nosso */
	/* preserva TODO estado que o C do host pode clobbrar e o guest espera intacto
	 * (o guest ve um svc inline, nao uma chamada AAPCS): x6/x7, NZCV, FPCR/FPSR */
	"  stp x6, x7, [sp, #80]\n"
	"  mrs x11, nzcv\n"
	"  mrs x12, fpcr\n"
	"  mrs x13, fpsr\n"
	"  stp x11, x12, [sp, #96]\n"
	"  str x13, [sp, #112]\n"
	"  mrs x9, tpidr_el0\n"         /* salva o TP do GUEST */
	"  str x9, [sp, #120]\n"
	"  adrp x10, g_host_tp\n"       /* carrega o TP do HOST e instala */
	"  ldr x10, [x10, #:lo12:g_host_tp]\n"
	"  msr tpidr_el0, x10\n"
	"  mov x0, sp\n"
	"  bl pc_do_syscall\n"          /* retorno em x0 (agora com TP do host) */
	"  ldr x9, [sp, #120]\n"        /* restaura o TP do GUEST */
	"  msr tpidr_el0, x9\n"
	"  ldp x11, x12, [sp, #96]\n"   /* restaura NZCV/FPCR/FPSR */
	"  ldr x13, [sp, #112]\n"
	"  msr nzcv, x11\n"
	"  msr fpcr, x12\n"
	"  msr fpsr, x13\n"
	"  ldp x6, x7, [sp, #80]\n"
	"  ldp x17, x30, [sp, #56]\n"
	"  add sp, sp, #160\n"
	"  br x17\n"                     /* volta pro guest apos o svc */
);
extern void pc_trampoline(void);

/*
 * Heap emulado para brk(): o LKL no-MMU nao inicializa mm->start_brk porque nao
 * fomos nos (nao o kernel) que carregamos o ELF, entao brk(0) retornaria 0 e a
 * glibc do guest segfaultaria. Reservamos uma regiao e servimos brk dela.
 */
#define GUEST_HEAP_SIZE (16 * 1024 * 1024)
static uint8_t *g_heap_base;   /* inicio da regiao */
static uint8_t *g_heap_cur;    /* fim atual (brk) */
static uint8_t *g_heap_end;    /* limite */
static uint64_t g_brk_hint;    /* onde o heap deve comecar (fim da imagem) */

static long emulate_brk(long addr)
{
	if (!g_heap_base) {
		/*
		 * Heap CONTIGUO logo apos a imagem do guest (como um brk real), com
		 * MAP_FIXED no endereco esperado. _dl_early_allocate compara brk(0)
		 * com brk(cur+size) e precisa que a regiao seja contigua e previsivel.
		 */
		void *want = (void *)((g_brk_hint + 0xFFF) & ~0xFFFUL);
		g_heap_base = mmap(want, GUEST_HEAP_SIZE,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS |
				   (g_brk_hint ? MAP_FIXED : 0), -1, 0);
		if (g_heap_base == MAP_FAILED)
			g_heap_base = mmap(NULL, GUEST_HEAP_SIZE,
					   PROT_READ | PROT_WRITE,
					   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		g_heap_cur = g_heap_base;
		g_heap_end = g_heap_base + GUEST_HEAP_SIZE;
	}
	if (addr == 0)                       /* consulta: retorna break atual */
		return (long)g_heap_cur;
	if ((uint8_t *)addr < g_heap_base || (uint8_t *)addr > g_heap_end) {
		fprintf(stderr, "[brk] RECUSADO addr=0x%lx (heap %p..%p)\n",
			addr, g_heap_base, g_heap_end);
		fflush(stderr);
		return (long)g_heap_cur;     /* fora da regiao: recusa (retorna atual) */
	}
	g_heap_cur = (uint8_t *)addr;        /* estende/encolhe */
	return (long)g_heap_cur;
}

long pc_do_syscall(long *regs)
{
	long nr = regs[6];
	long params[6] = { regs[0], regs[1], regs[2], regs[3], regs[4], regs[5] };
	long ret;

	/*
	 * CONSOLE: os fds 0/1/2 (stdin/stdout/stderr) nao existem no LKL, entao
	 * write(1,...) dava EBADF e a glibc segfaultava no caminho de erro. Ligamos
	 * esses fds ao console do HOST (no Switch: a tela). Interceptamos as syscalls
	 * de fd que operam em 0/1/2. Estas sao a "peca do terminal" da fase 8.
	 */
	if ((nr == 64 /*write*/ ) && (regs[0] == 1 || regs[0] == 2)) {
		long w = write(regs[0], (void *)regs[1], regs[2]);
		return w < 0 ? -5 : w;
	}
	if (nr == 63 /*read*/ && regs[0] == 0) {
		long r = read(0, (void *)regs[1], regs[2]);
		return r < 0 ? -5 : r;
	}
	if (nr == 79 /*newfstatat*/ && (regs[0] == 0 || regs[0] == 1 || regs[0] == 2)) {
		/* fstat do HOST no fd de console (layout struct stat aarch64 identico) */
		return fstat(regs[0], (void *)regs[2]) < 0 ? -9 : 0;
	}
	if (nr == 29 /*ioctl*/ && (regs[0] == 0 || regs[0] == 1 || regs[0] == 2))
		return 0;  /* finge sucesso em TCGETS etc (e um tty) */

	/* intercepta syscalls que o LKL nao serve bem no nosso modelo de carga */
	if (nr == 214) {                     /* brk */
		ret = emulate_brk(regs[0]);
		fprintf(stderr, "[trampolim] brk(0x%lx) -> 0x%lx\n", regs[0], ret);
		fflush(stderr);
		return ret;
	}
	/*
	 * Syscalls de init da glibc que o LKL retorna ENOSYS mas cujo resultado nao
	 * afeta 'ls': fingimos sucesso pra a glibc nao abortar. mprotect no no-MMU
	 * nao faz nada util (sem paginacao); set_robust_list/rseq idem.
	 */
	switch (nr) {
	case 226: /* mprotect */
	case 99:  /* set_robust_list */
	case 293: /* rseq */
	case 135: /* rt_sigprocmask (se aparecer) */
		fprintf(stderr, "[trampolim] stub nr=%ld -> 0\n", nr);
		fflush(stderr);
		return 0;
	case 222: { /* mmap: emula com memoria REAL do host (anonimo).
		     * O mmap do LKL retorna endereco do espaco interno dele que o
		     * guest nao consegue tocar -> emulamos com mmap do host. */
		long len = regs[1], prot = regs[2], flags = regs[3], gfd = regs[4];
		void *m;

		if (gfd == -1 || (flags & 0x20)) { /* MAP_ANONYMOUS */
			m = mmap(NULL, len, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			fprintf(stderr, "[trampolim] mmap(len=0x%lx) -> %p\n",
				len, m);
			fflush(stderr);
			return (m == MAP_FAILED) ? -12 : (long)m;
		}
		/* mmap de arquivo: passa pro LKL (raro no ls) */
		break;
	}
	}

	ret = lkl_syscall(nr, params);
	fprintf(stderr, "[trampolim] nr=%ld a0=%ld -> %ld\n", nr, regs[0], ret);
	fflush(stderr);
	return ret;
}

struct eh { unsigned char id[16]; uint16_t type, machine; uint32_t ver;
	    uint64_t entry, phoff, shoff; uint32_t flags;
	    uint16_t ehsz, phentsz, phnum, shentsz, shnum, shstrndx; };
struct ph { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct sh { uint32_t name, type; uint64_t flags, addr, off, size;
	    uint32_t link, info; uint64_t align, entsize; };
struct dyn { int64_t tag; uint64_t val; };
struct rela { uint64_t off, info, addend; };

#define PT_DYNAMIC 2
#define DT_RELA 7
#define DT_RELASZ 8
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_IRELATIVE 1032

/*
 * Aplica R_AARCH64_IRELATIVE (IFUNC): binarios estaticos (busybox) resolvem
 * memcpy/strlen otimizados chamando um resolver cujo endereco esta no addend; o
 * retorno vai no slot da GOT. Sem isso, a GOT tem lixo e a 1a chamada via PLT
 * pula pra lixo -> segfault. Le a .rela.plt pela section header (nao ha
 * PT_DYNAMIC em binario estatico). EXEC nao-PIE: enderecos absolutos (lo=0 rebase).
 */
static void apply_irelative(uint8_t *file, uint8_t *img, uint64_t lo)
{
	struct eh *e = (struct eh *)file;
	struct sh *sh = (struct sh *)(file + e->shoff);
	const char *shstr = (const char *)(file + sh[e->shstrndx].off);

	for (int i = 0; i < e->shnum; i++) {
		if (strcmp(shstr + sh[i].name, ".rela.plt") != 0)
			continue;
		struct rela *r = (struct rela *)(img + (sh[i].addr - lo));
		int n = sh[i].size / sizeof(struct rela);

		for (int k = 0; k < n; k++) {
			if ((r[k].info & 0xffffffff) != R_AARCH64_IRELATIVE)
				continue;
			/* chama o resolver (no img) e grava o retorno no slot */
			uintptr_t resolver = (uintptr_t)(img + (r[k].addend - lo));
			long (*fn)(void) = (long (*)(void))resolver;
			uint64_t *slot = (uint64_t *)(img + (r[k].off - lo));
			*slot = (uint64_t)fn();
		}
	}
}

/* aplica R_AARCH64_RELATIVE: *(base+off) = base + addend. img=base de carga,
 * lo=menor vaddr. Necessario para PIE (static-pie) rodar fora do vaddr do ELF. */
static void apply_relocs(uint8_t *img, uint64_t lo, struct ph *p, int phnum)
{
	for (int i = 0; i < phnum; i++) {
		if (p[i].type != PT_DYNAMIC) continue;
		struct dyn *d = (struct dyn *)(img + (p[i].vaddr - lo));
		uint64_t rela_off = 0, rela_sz = 0;

		for (; d->tag != 0; d++) {
			if (d->tag == DT_RELA) rela_off = d->val;
			else if (d->tag == DT_RELASZ) rela_sz = d->val;
		}
		if (!rela_off) continue;
		struct rela *r = (struct rela *)(img + (rela_off - lo));
		int n = rela_sz / sizeof(struct rela);

		for (int k = 0; k < n; k++) {
			if ((r[k].info & 0xffffffff) != R_AARCH64_RELATIVE) continue;
			uint64_t *slot = (uint64_t *)(img + (r[k].off - lo));
			*slot = (uint64_t)img + (r[k].addend - lo);
		}
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) { printf("uso: exec-hw <guest.elf>\n"); return 2; }

	if (lkl_init(&lkl_host_ops)) return 1;
	if (lkl_start_kernel("mem=64M loglevel=8")) return 1;
	lkl_sys_mkdirat(LKL_AT_FDCWD, "/tmp", 0755);
	lkl_sys_mount("tmpfs", "/tmp", "tmpfs", 0, NULL);

	FILE *f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *file = malloc(fsz);
	if (fread(file, 1, fsz, f) != (size_t)fsz) return 1;
	fclose(f);

	struct eh *e = (struct eh *)file;
	struct ph *p = (struct ph *)(file + e->phoff);
	uint64_t lo = ~0ULL, hi = 0;
	for (int i = 0; i < e->phnum; i++) {
		if (p[i].type != 1) continue;
		if (p[i].vaddr < lo) lo = p[i].vaddr;
		if (p[i].vaddr + p[i].memsz > hi) hi = p[i].vaddr + p[i].memsz;
	}
	size_t span = (hi - lo + VENEER_POOL + 0xFFF) & ~0xFFFUL; /* +pool de veneers */

	/*
	 * ELF EXEC (nao-PIE, ex: busybox estatico) usa enderecos absolutos e SO roda
	 * carregado no seu vaddr (lo). Mapeamos FIXO em `lo`. ELF DYN (PIE) roda em
	 * qualquer base -> mmap arbitrario + relocs. e->type: 2=EXEC, 3=DYN.
	 */
	int is_pie = (e->type == 3);
	uint8_t *img;
	if (is_pie) {
		img = mmap(NULL, span, PROT_READ | PROT_WRITE | PROT_EXEC,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	} else {
		img = mmap((void *)lo, span, PROT_READ | PROT_WRITE | PROT_EXEC,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (img == MAP_FAILED) { perror("mmap FIXED"); return 1; }
	}
	memset(img, 0, span);
	for (int i = 0; i < e->phnum; i++) {
		if (p[i].type != 1) continue;
		memcpy(img + (p[i].vaddr - lo), file + p[i].off, p[i].filesz);
	}

	if (is_pie)
		apply_relocs(img, lo, p, e->phnum);

	/*
	 * Um veneer POR svc, numa area no fim do img (perto do codigo -> `b` de 26
	 * bits alcanca). Cada svc vira `b veneer_i` (branch simples: NAO grava x30,
	 * preserva o x30 do guest). O veneer_i carrega x17 = endereco apos aquele svc
	 * e salta (br) pro pc_trampoline, que volta via x17. 6 instrucoes por veneer.
	 */
	uint32_t *code = (uint32_t *)img;
	uint32_t *vpool = (uint32_t *)(img + span - VENEER_POOL);
	int npatch = 0, vi = 0;

	for (size_t i = 0; i < (span - VENEER_POOL) / 4; i++) {
		if (code[i] != SVC0) continue;
		uint32_t *v = &vpool[vi * 8];       /* 8 words por veneer (32 bytes) */
		uintptr_t ret_addr = (uintptr_t)&code[i] + 4; /* apos o svc */

		/* veneer: x17=ret_addr (ldr do literal); x16=trampoline; br x16 */
		v[0] = 0x58000091u; /* ldr x17, #16  -> ret_addr (literal em v[4]) */
		v[1] = 0x580000B0u; /* ldr x16, #20  -> trampoline (literal em v[6]) */
		v[2] = 0xD61F0200u; /* br  x16 */
		v[3] = 0xD503201Fu; /* nop (pad) */
		*(uint64_t *)&v[4] = (uint64_t)ret_addr;
		*(uint64_t *)&v[6] = (uint64_t)pc_trampoline;

		/* patch: svc -> b veneer (branch sem link) */
		intptr_t off = (intptr_t)((uintptr_t)v - (uintptr_t)&code[i]);
		code[i] = 0x14000000u | (((int32_t)(off >> 2)) & 0x03FFFFFFu);
		npatch++; vi++;
	}
	printf("patch: %d svc->bl, entry off=0x%lx\n", npatch,
	       (unsigned long)(e->entry - lo));
	fflush(stdout);

	__builtin___clear_cache((char *)img, (char *)img + span);

	/* IFUNC: resolve memcpy/strlen otimizados (roda codigo do guest, entao
	 * DEPOIS do patch de svc e do clear_cache). Sem isso a GOT tem lixo. */
	if (!is_pie)
		apply_irelative(file, img, lo);

	uintptr_t entry = (uintptr_t)(img + (e->entry - lo));
	printf("img=%p entry=%p trampoline=%p\n", img, (void *)entry,
	       (void *)pc_trampoline);
	g_brk_hint = lo + span;   /* heap comeca logo apos a imagem */
	printf("DEBUG lo=0x%lx hi=0x%lx span=0x%lx brk_hint=0x%lx\n",
	       lo, hi, span, g_brk_hint);
	/* testa se a pagina do .bss (ex 0x496000) esta acessivel */
	if (0x496000 >= lo && 0x496000 < lo + span) {
		volatile uint8_t *probe = (uint8_t *)0x496038;
		printf("DEBUG probe 0x496038 = 0x%02x (acessivel)\n", *probe);
	}

	/*
	 * Monta o STACK DE PROCESSO Linux que a glibc/_start do busybox espera.
	 * Layout (do baixo pro alto endereco, sp aponta pro argc):
	 *   [argc][argv0]..[argvN][NULL][envp..][NULL][auxv pares][AT_NULL]
	 * argv/argstrings do guest = os args passados apos o binario.
	 */
	static uint64_t gstack[4096];             /* stack do guest, 32KB */
	int gargc = argc - 2;                     /* args apos "exec-hw <bin>" */
	if (gargc < 1) gargc = 1;                 /* ao menos argv0 */
	char *argv0 = argv[1];                    /* nome do binario */

	uint64_t *sp = &gstack[2048];             /* comeca no meio, cresce p/ baixo */
	/*
	 * auxv COMPLETO — a glibc estatica le varias entradas AT_* pra se inicializar
	 * (achar os program headers, uid, hwcap, random). Com auxv minimo ela calcula
	 * ponteiros errados e segfaulta. Valores conferidos contra o que o qemu-user
	 * puro passa (onde o mesmo busybox roda ok).
	 */
	static uint64_t rnd[2] = { 0x0123456789abcdefULL, 0xfedcba9876543210ULL };
	uint64_t phdr_va = (uint64_t)img + e->phoff; /* PHDR na imagem carregada */
	/* HWCAP: le do proprio host (nosso hwcap aarch64 real). Passar 0 fazia a
	 * glibc escolher caminhos de codigo inconsistentes. */
	unsigned long hwcap = getauxval(16);   /* AT_HWCAP */
	unsigned long hwcap2 = getauxval(26);  /* AT_HWCAP2 */
	#define AUX(t, v) do { *(--sp) = (v); *(--sp) = (t); } while (0)
	AUX(0, 0);                    /* AT_NULL */
	AUX(23, 0);                   /* AT_SECURE = 0 */
	AUX(25, (uint64_t)rnd);       /* AT_RANDOM */
	AUX(26, hwcap2);              /* AT_HWCAP2 */
	AUX(16, hwcap);               /* AT_HWCAP (real do host) */
	AUX(17, 100);                 /* AT_CLKTCK */
	AUX(6, 4096);                 /* AT_PAGESZ */
	AUX(11, 1000);                /* AT_UID */
	AUX(12, 1000);                /* AT_EUID */
	AUX(13, 1000);                /* AT_GID */
	AUX(14, 1000);                /* AT_EGID */
	AUX(8, 0);                    /* AT_FLAGS */
	AUX(7, 0);                    /* AT_BASE (sem interpretador) */
	AUX(9, entry);                /* AT_ENTRY */
	AUX(5, e->phnum);             /* AT_PHNUM */
	AUX(4, e->phentsz);           /* AT_PHENT */
	AUX(3, phdr_va);              /* AT_PHDR */
	#undef AUX
	*(--sp) = 0;                                           /* envp NULL */
	/* argv[]: argv0 + os args do usuario (argv[2..]) */
	uint64_t *argv_slots = sp - (gargc + 1);
	argv_slots[gargc] = 0;                                 /* argv NULL term */
	argv_slots[0] = (uint64_t)argv0;
	for (int a = 1; a < gargc; a++)
		argv_slots[a] = (uint64_t)argv[a + 1];
	sp = argv_slots;
	*(--sp) = gargc;                                       /* argc */

	/*
	 * ABI aarch64: sp DEVE estar alinhado a 16 no entry. Se cair em multiplo de 8
	 * mas nao 16, a glibc le argv/envp/auxv deslocados de 8 -> pega o AT_RANDOM
	 * como ponteiro de funcao e pula pra ele (executa a pilha). Alinha pra baixo
	 * movendo tudo 8 bytes se necessario (argc precisa continuar no topo).
	 */
	if ((uintptr_t)sp & 8) {
		uint64_t *nsp = sp - 1;             /* desce 8 -> alinha a 16 */
		int words = (&gstack[2048]) - sp;   /* qtde de palavras empilhadas */
		for (int w = 0; w < words; w++)
			nsp[w] = sp[w];             /* desloca o bloco inteiro */
		sp = nsp;
	}

	printf("stack montado: sp=%p (%%16=%lu) argc=%d argv0=%s\n",
	       (void *)sp, (uintptr_t)sp % 16, gargc, argv0);
	printf("DEBUG AT_PHDR=0x%lx (qemu usa 0x400040) e_phoff=0x%lx phnum=%d\n",
	       phdr_va, (unsigned long)e->phoff, e->phnum);
	/* dump dos primeiros 16 words da pilha, como o gdb faria */
	printf("DEBUG stack[sp..]:\n");
	fflush(stdout);
	printf("saltando no guest...\n"); fflush(stdout);

	/* captura o TP do HOST antes de entrar no guest — o trampolim vai restaurar
	 * este valor sempre que rodar C do host (ver pc_trampoline). */
	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(g_host_tp));

	/* salta pro entry com SP = nosso stack de processo. asm: nao retorna a nao
	 * ser que o guest faca ret (busybox faz exit-syscall, entao nao volta aqui). */
	register uint64_t x_sp __asm__("x9") = (uint64_t)sp;
	register uint64_t x_en __asm__("x10") = entry;
	__asm__ volatile(
		"mov x19, sp\n"      /* preserva nosso sp do host em callee-saved */
		"mov sp, %0\n"
		"blr %1\n"
		"mov sp, x19\n"       /* se o guest retornar, restaura */
		: : "r"(x_sp), "r"(x_en)
		: "x19", "x30", "memory");

	printf("guest retornou ao host\n"); fflush(stdout);
	lkl_sys_halt();
	printf("=== guest executou (ver saida acima) ===\n");
	return 0;
}
