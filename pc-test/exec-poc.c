/*
 * exec-poc.c - prova de conceito do EXECUTOR DE BINARIOS (fase 7) sobre LKL.
 *
 * O nucleo dificil do executor e a PONTE DE ABI: quando o binario guest executa
 * `svc #0`, a CPU tem x8=numero-da-syscall e x0..x5=args (ABI Linux/aarch64).
 * Nosso trampolim traduz isso numa chamada lkl_syscall(x8, {x0..x5}) e devolve
 * o retorno em x0. Este PoC exercita essa ponte com um "programa" representado
 * como lista de syscalls (no Switch, o svc real vem via handler de excecao +
 * memoria jitCreate; a traducao ABI e IDENTICA e e o que validamos aqui).
 *
 * Prova: um guest faz openat/write/close/exit exatamente como um ELF faria,
 * e o efeito colateral (arquivo criado no VFS do kernel) e verificado.
 */
#include <stdio.h>
#include <string.h>
#include <lkl.h>
#include <lkl_host.h>

/* registradores ARM64 relevantes pra uma syscall */
struct guest_regs {
	long x[8]; /* x0..x5 args, x6/x7 nao usados, mas mantemos alinhado */
	long x8;   /* numero da syscall */
};

/* trampolim: o que o handler de svc #0 fara no hardware.
 * traduz os registradores do guest -> lkl_syscall -> retorno em x0. */
static long svc_trampoline(struct guest_regs *r)
{
	long params[6] = { r->x[0], r->x[1], r->x[2], r->x[3], r->x[4], r->x[5] };
	long ret = lkl_syscall(r->x8, params);

	r->x[0] = ret; /* ABI: retorno vai pra x0 */
	return ret;
}

/* uma "instrucao svc" do nosso guest-brinquedo */
struct guest_op {
	long nr;
	long a0, a1, a2, a3;
};

int main(void)
{
	int ret;
	char readback[64] = {0};

	ret = lkl_init(&lkl_host_ops);
	if (ret) { printf("lkl_init=%d\n", ret); return 1; }
	ret = lkl_start_kernel("mem=64M loglevel=8");
	if (ret) { printf("start_kernel=%d\n", ret); return 1; }

	/* --- monta tmpfs onde o "programa" vai escrever --- */
	lkl_sys_mkdirat(LKL_AT_FDCWD, "/tmp", 0755);
	lkl_sys_mount("tmpfs", "/tmp", "tmpfs", 0, NULL);

	/*
	 * "Programa" guest: equivale ao C
	 *   fd = openat(AT_FDCWD,"/tmp/out",O_RDWR|O_CREAT,0644);
	 *   write(fd,"executado via ponte ABI!",25);
	 *   close(fd);
	 * expresso como as syscalls cruas que um ELF emitiria via svc #0.
	 */
	static const char path[] = "/tmp/out";
	static const char msg[]  = "executado via ponte ABI!";
	struct guest_regs regs = {0};
	long guest_fd;

	/* openat */
	regs.x8 = __lkl__NR_openat;
	regs.x[0] = LKL_AT_FDCWD;
	regs.x[1] = (long)path;
	regs.x[2] = LKL_O_RDWR | LKL_O_CREAT;
	regs.x[3] = 0644;
	svc_trampoline(&regs);
	guest_fd = regs.x[0];
	printf("guest openat -> fd=%ld\n", guest_fd);

	/* write */
	regs.x8 = __lkl__NR_write;
	regs.x[0] = guest_fd;
	regs.x[1] = (long)msg;
	regs.x[2] = sizeof(msg) - 1;
	svc_trampoline(&regs);
	printf("guest write -> %ld bytes\n", regs.x[0]);

	/* close */
	regs.x8 = __lkl__NR_close;
	regs.x[0] = guest_fd;
	svc_trampoline(&regs);

	/* --- host verifica o efeito colateral no VFS do kernel --- */
	int hfd = lkl_sys_openat(LKL_AT_FDCWD, "/tmp/out", LKL_O_RDONLY, 0);
	long n = lkl_sys_read(hfd, readback, sizeof(readback) - 1);
	lkl_sys_close(hfd);
	printf("host leu de /tmp/out (%ld): %s\n", n, readback);

	lkl_sys_halt();

	if (n == (long)(sizeof(msg) - 1) && strcmp(readback, msg) == 0) {
		printf("=== PONTE ABI OK: guest 'executou' via lkl_syscall ===\n");
		return 0;
	}
	printf("=== FALHOU ===\n");
	return 1;
}
