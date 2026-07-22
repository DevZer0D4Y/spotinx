/*
 * elf-loader.c - carregador de ELF aarch64 estatico (metade "loader" da fase 7).
 *
 * Faz o parse dos program headers de um ELF64 e mapeia os segmentos PT_LOAD em
 * memoria, montando a imagem de execucao do guest. No Switch a memoria de
 * destino vem de jitCreate (RX); aqui usamos um buffer para VALIDAR o parsing,
 * os offsets, o BSS zerado e o calculo do entry point — a logica e a mesma.
 *
 * Combinado com a ponte de ABI (exec-poc.c), forma o executor completo:
 *   load_elf() posiciona o codigo -> entra no entry -> cada svc #0 -> trampolim.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PT_LOAD 1
#define PF_X 1

struct elf64_ehdr {
	unsigned char e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf64_phdr {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

struct loaded_elf {
	uint8_t *image;      /* base da imagem mapeada */
	uint64_t base_vaddr; /* menor p_vaddr (para ELF nao-PIE, o vaddr real) */
	uint64_t span;       /* tamanho total mapeado */
	uint64_t entry;      /* offset do entry dentro de image */
	int has_x;           /* achou segmento executavel */
};

/* le arquivo inteiro pra um buffer malloc'd */
static uint8_t *slurp(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(*size);
	if (b && fread(b, 1, *size, f) != (size_t)*size) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

int load_elf(const char *path, struct loaded_elf *out)
{
	long fsz;
	uint8_t *file = slurp(path, &fsz);
	struct elf64_ehdr *eh;
	struct elf64_phdr *ph;
	uint64_t lo = ~0ULL, hi = 0;
	int i;

	if (!file) { printf("nao abriu %s\n", path); return -1; }
	eh = (struct elf64_ehdr *)file;

	if (memcmp(eh->e_ident, "\x7f""ELF", 4) != 0) {
		printf("nao e ELF\n"); free(file); return -1;
	}
	if (eh->e_ident[4] != 2) { printf("nao e ELF64\n"); free(file); return -1; }
	if (eh->e_machine != 183) { /* EM_AARCH64 */
		printf("nao e aarch64 (machine=%u)\n", eh->e_machine);
		free(file); return -1;
	}

	ph = (struct elf64_phdr *)(file + eh->e_phoff);

	/* 1a passada: extensao de enderecos dos PT_LOAD */
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (ph[i].p_vaddr < lo)
			lo = ph[i].p_vaddr;
		if (ph[i].p_vaddr + ph[i].p_memsz > hi)
			hi = ph[i].p_vaddr + ph[i].p_memsz;
	}
	if (hi <= lo) { printf("sem PT_LOAD\n"); free(file); return -1; }

	memset(out, 0, sizeof(*out));
	out->base_vaddr = lo;
	out->span = hi - lo;
	out->image = calloc(1, out->span); /* calloc => BSS ja vem zerado */
	if (!out->image) { free(file); return -1; }

	/* 2a passada: copia filesz de cada segmento no offset relativo a base */
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		memcpy(out->image + (ph[i].p_vaddr - lo),
		       file + ph[i].p_offset, ph[i].p_filesz);
		if (ph[i].p_flags & PF_X)
			out->has_x = 1;
		printf("  PT_LOAD vaddr=0x%lx filesz=0x%lx memsz=0x%lx %s\n",
		       (unsigned long)ph[i].p_vaddr,
		       (unsigned long)ph[i].p_filesz,
		       (unsigned long)ph[i].p_memsz,
		       (ph[i].p_flags & PF_X) ? "[X]" : "");
	}

	out->entry = eh->e_entry - lo;
	printf("  entry=0x%lx (offset 0x%lx na imagem)\n",
	       (unsigned long)eh->e_entry, (unsigned long)out->entry);
	free(file);
	return 0;
}

int main(int argc, char **argv)
{
	struct loaded_elf le;

	if (argc < 2) {
		printf("uso: elf-loader <binario-aarch64-estatico>\n");
		return 2;
	}
	printf("carregando %s:\n", argv[1]);
	if (load_elf(argv[1], &le) != 0)
		return 1;

	printf("imagem: base=0x%lx span=%lu bytes, executavel=%s\n",
	       (unsigned long)le.base_vaddr, (unsigned long)le.span,
	       le.has_x ? "sim" : "nao");
	/* checagem: primeira instrucao no entry nao pode ser zero (=guard) */
	uint32_t first = *(uint32_t *)(le.image + le.entry);
	printf("1a instrucao no entry: 0x%08x\n", first);
	if (le.has_x && first != 0) {
		printf("=== LOADER OK: imagem pronta para saltar no entry ===\n");
		free(le.image);
		return 0;
	}
	printf("=== FALHOU ===\n");
	free(le.image);
	return 1;
}
