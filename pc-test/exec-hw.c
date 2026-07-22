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
__asm__(
	".text\n"
	".global pc_trampoline\n"
	".balign 4\n"
	"pc_trampoline:\n"
	"  sub sp, sp, #80\n"
	"  stp x0, x1, [sp, #0]\n"
	"  stp x2, x3, [sp, #16]\n"
	"  stp x4, x5, [sp, #32]\n"
	"  str x8, [sp, #48]\n"
	"  stp x17, x30, [sp, #56]\n"  /* x17=retorno guest, x30=preserva o nosso */
	"  mov x0, sp\n"
	"  bl pc_do_syscall\n"          /* retorno em x0 */
	"  ldp x17, x30, [sp, #56]\n"   /* restaura */
	"  add sp, sp, #80\n"
	"  br x17\n"                     /* volta pro guest apos o svc; x30 intacto */
);
extern void pc_trampoline(void);

long pc_do_syscall(long *regs)
{
	long params[6] = { regs[0], regs[1], regs[2], regs[3], regs[4], regs[5] };
	long ret;

	fprintf(stderr, "[trampolim] nr=%ld a0=%ld a1=0x%lx a2=0x%lx\n",
		regs[6], regs[0], regs[1], regs[2]);
	fflush(stderr);
	ret = lkl_syscall(regs[6], params);
	fprintf(stderr, "[trampolim] nr=%ld -> %ld\n", regs[6], ret);
	fflush(stderr);
	return ret;
}

struct eh { unsigned char id[16]; uint16_t type, machine; uint32_t ver;
	    uint64_t entry, phoff, shoff; uint32_t flags;
	    uint16_t ehsz, phentsz, phnum, shentsz, shnum, shstrndx; };
struct ph { uint32_t type, flags; uint64_t off, vaddr, paddr, filesz, memsz, align; };
struct dyn { int64_t tag; uint64_t val; };
struct rela { uint64_t off, info, addend; };

#define PT_DYNAMIC 2
#define DT_RELA 7
#define DT_RELASZ 8
#define R_AARCH64_RELATIVE 1027

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

	uint8_t *img = mmap(NULL, span, PROT_READ | PROT_WRITE | PROT_EXEC,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(img, 0, span);
	for (int i = 0; i < e->phnum; i++) {
		if (p[i].type != 1) continue;
		memcpy(img + (p[i].vaddr - lo), file + p[i].off, p[i].filesz);
	}

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

	void (*entry)(void) = (void (*)(void))(img + (e->entry - lo));
	printf("img=%p entry=%p trampoline=%p\n", img, (void *)entry,
	       (void *)pc_trampoline);
	fflush(stdout);
	printf("saltando no guest...\n"); fflush(stdout);
	entry();
	printf("guest retornou ao host\n"); fflush(stdout);

	/* host verifica o efeito colateral */
	char buf[64] = {0};
	int fd = lkl_sys_openat(LKL_AT_FDCWD, "/tmp/guest-out", LKL_O_RDONLY, 0);
	long n = lkl_sys_read(fd, buf, sizeof(buf) - 1);
	lkl_sys_close(fd);
	lkl_sys_halt();

	printf("host leu /tmp/guest-out (%ld): %s\n", n, buf);
	if (n > 0 && strstr(buf, "guest executou")) {
		printf("=== FASE 7 (cadeia completa) OK NO PC ===\n");
		return 0;
	}
	printf("=== FALHOU ===\n");
	return 1;
}
