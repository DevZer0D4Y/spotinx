/*
 * guest.c - binario guest de teste da fase 7 (aarch64 estatico, freestanding).
 * Roda DENTRO do LKL via nx_exec: cada svc #0 vira bl->trampolim->lkl_syscall.
 * Prova: abre /tmp/guest-out, escreve uma marca, fecha. O host le de volta.
 *
 * Numeros de syscall aarch64: openat=56, write=64, close=57, exit=93.
 * AT_FDCWD = -100. O /tmp e montado (tmpfs) pelo host antes de executar.
 */
static long sys(long nr, long a0, long a1, long a2, long a3)
{
	register long x8 __asm__("x8") = nr;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
			 : "memory");
	return x0;
}

#define AT_FDCWD   (-100)
#define O_WRONLY   1
#define O_CREAT    0100

/* PIE: literais lidos via PC-relativo; relocacoes R_AARCH64_RELATIVE aplicadas
 * pelo loader (nx_exec/exec-hw) na carga. */
static const char path[] = "/tmp/guest-out";
static const char msg[]  = "guest executou via nx_exec!";

void _start(void)
{
	long fd = sys(56, AT_FDCWD, (long)path, O_WRONLY | O_CREAT, 0644);

	if (fd >= 0) {
		sys(64, fd, (long)msg, sizeof(msg) - 1, 0);  /* write */
		sys(57, fd, 0, 0, 0);                        /* close */
	}
	/* sem exit: LKL nao mata processos. Retornamos ao host via ret (o _start
	 * foi chamado por entry(), entao o ret volta pro exec-hw). */
}
