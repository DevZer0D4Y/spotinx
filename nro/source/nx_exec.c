/*
 * nx_exec.c - fase 7: executor de binarios ELF aarch64 no Switch.
 *
 * Cadeia VALIDADA no PC (qemu-aarch64, pc-test/exec-hw.c). Aqui a unica diferenca
 * e a origem da memoria RX: jitCreate (Switch) no lugar de mmap(PROT_EXEC).
 *
 *   1. load ELF PIE: parse PT_LOAD -> imagem
 *   2. jitCreate: memoria RW->RX
 *   3. apply_relocs: R_AARCH64_RELATIVE (PIE roda em qualquer base)
 *   4. veneer-por-svc: cada `svc #0` -> `b veneer` (branch SEM link: preserva o
 *      x30 do guest); veneer carrega x17=retorno e salta pro trampolim
 *   5. salta no entry; cada syscall -> trampolim (nx_trampoline.s) -> lkl_syscall
 *
 * ALVO INICIAL: guest minimo (openat/write/close). busybox depois.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <switch.h>
#include <lkl.h>
#include <lkl_host.h>

#define SVC0_INSN   0xD4000001u
#define VENEER_POOL 0x4000  /* 16KB p/ veneers (32B cada) */
#define R_AARCH64_RELATIVE 1027
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define DT_RELA 7
#define DT_RELASZ 8

extern void nx_svc_trampoline(void); /* nx_trampoline.s */

/* chamado pelo trampolim asm: regs[0..5]=args, regs[6]=nr */
long nx_do_syscall(long *regs)
{
	long params[6] = { regs[0], regs[1], regs[2], regs[3], regs[4], regs[5] };

	return lkl_syscall(regs[6], params);
}

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

static void apply_relocs(uint8_t *img, uint64_t lo, struct e64_phdr *p, int phnum)
{
	for (int i = 0; i < phnum; i++) {
		if (p[i].p_type != PT_DYNAMIC)
			continue;
		struct e64_dyn *d = (struct e64_dyn *)(img + (p[i].p_vaddr - lo));
		uint64_t rela_off = 0, rela_sz = 0;

		for (; d->tag != 0; d++) {
			if (d->tag == DT_RELA) rela_off = d->val;
			else if (d->tag == DT_RELASZ) rela_sz = d->val;
		}
		if (!rela_off)
			continue;
		struct e64_rela *r = (struct e64_rela *)(img + (rela_off - lo));
		int n = rela_sz / sizeof(struct e64_rela);

		for (int k = 0; k < n; k++) {
			if ((r[k].info & 0xffffffff) != R_AARCH64_RELATIVE)
				continue;
			uint64_t *slot = (uint64_t *)(img + (r[k].off - lo));
			*slot = (uint64_t)img + (r[k].addend - lo);
		}
	}
}

/* carrega, patcha e executa. Retorna 0 ok, <0 erro. */
int nx_exec_elf(const char *path)
{
	FILE *f = fopen(path, "rb");
	long fsz;
	uint8_t *file;
	struct e64_ehdr *eh;
	struct e64_phdr *ph;
	uint64_t lo = ~0ULL, hi = 0;
	Jit jit;
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
	if (R_FAILED(jitCreate(&jit, span))) {
		printf("nx_exec: jitCreate falhou\n"); free(file); return -1;
	}
	jitTransitionToWritable(&jit);
	uint8_t *rw = (uint8_t *)jit.rw_addr;
	uint8_t *rx = (uint8_t *)jit.rx_addr;

	memset(rw, 0, span);
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD) continue;
		memcpy(rw + (ph[i].p_vaddr - lo), file + ph[i].p_offset, ph[i].p_filesz);
	}

	apply_relocs(rw, lo, ph, eh->e_phnum);

	/*
	 * Veneer-por-svc (validado no PC): svc -> `b veneer` (sem link, preserva x30).
	 * Enderecos dos veneers e do retorno sao no espaco RX (onde vai executar),
	 * mas escrevemos via ponteiro RW. rx = base de execucao.
	 */
	uint32_t *code_rw = (uint32_t *)rw;
	uint32_t *vpool_rw = (uint32_t *)(rw + span - VENEER_POOL);
	uintptr_t vpool_rx = (uintptr_t)(rx + span - VENEER_POOL);

	for (size_t k = 0; k < (span - VENEER_POOL) / 4; k++) {
		if (code_rw[k] != SVC0_INSN) continue;
		uint32_t *v = &vpool_rw[vi * 8];
		uintptr_t v_rx = vpool_rx + vi * 32;
		uintptr_t ret_rx = (uintptr_t)(rx + k * 4 + 4); /* apos o svc, em RX */

		v[0] = 0x58000091u; /* ldr x17, #16 (ret) */
		v[1] = 0x580000B0u; /* ldr x16, #20 (trampoline) */
		v[2] = 0xD61F0200u; /* br x16 */
		v[3] = 0xD503201Fu; /* nop */
		*(uint64_t *)&v[4] = (uint64_t)ret_rx;
		*(uint64_t *)&v[6] = (uint64_t)nx_svc_trampoline;

		intptr_t off = (intptr_t)(v_rx - (uintptr_t)(rx + k * 4));
		code_rw[k] = 0x14000000u | (((int32_t)(off >> 2)) & 0x03FFFFFFu);
		npatch++; vi++;
	}
	printf("nx_exec: %d svc->veneer, entry=0x%lx\n", npatch,
	       (unsigned long)(eh->e_entry - lo));

	jitTransitionToExecutable(&jit);
	armDCacheClean(jit.rw_addr, span);
	armICacheInvalidate(jit.rx_addr, span);

	void (*entry)(void) = (void (*)(void))(rx + (eh->e_entry - lo));
	free(file);
	printf("nx_exec: saltando no guest...\n");
	entry();
	printf("nx_exec: guest retornou\n");
	jitClose(&jit);
	return 0;
}
