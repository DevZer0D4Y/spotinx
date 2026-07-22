/*
 * lkl-switch: boota o kernel Linux (LKL) dentro de um NRO no Switch.
 * Fase 2: meta = ver o dmesg na tela.
 */
#include <stdio.h>
#include <switch.h>

#include <lkl_host.h>

int nx_exec_elf(const char *path);  /* nx_exec.c */
int nx_exec_argc;                   /* argv do guest, setado antes de nx_exec_elf */
char **nx_exec_argv;

/* console: o guest (busybox) escreve no fd 1/2 -> aparece na tela via printf.
 * Chamado pela ponte de syscall (nx_do_syscall). */
void nx_console_write(const char *s, int len)
{
	fwrite(s, 1, len, stdout);
}

/* marcador de estagio em arquivo: sobrevive a travamento/crash */
static void stage(const char *s)
{
	FILE *f = fopen("sdmc:/lkl-stage.log", "a");

	if (f) {
		fprintf(f, "%s\n", s);
		fclose(f);
	}
}

/*
 * Thread UNICA dona da tela: e a unica que chama consoleUpdate() (submissao de
 * framebuffer via libnx, nao thread-safe). Durante lkl_start_kernel() a main
 * thread fica bloqueada, entao sem esta thread o dmesg do boot nao apareceria.
 * As threads do kernel so escrevem no buffer de texto via nx_print.
 */
static int display_stop;
volatile int nx_display_pause;

static void nx_display_thread(void *arg)
{
	while (!display_stop) {
		if (!nx_display_pause)
			consoleUpdate(NULL);
		svcSleepThread(16666666ULL); /* ~60 Hz */
	}
}

/*
 * Todo o LKL roda NESTA thread (criada por threadCreate), nunca na main.
 * Motivo: threadGetSelf() na thread principal do Horizon retorna um Thread*
 * placeholder/inconsistente — a main nao foi criada por threadCreate. O LKL usa
 * thread_self()/thread_equal() na transferencia final de posse da CPU (o
 * /init -> sem_up -> lkl_cpu_change_owner), e se a identidade da main nao bate,
 * lkl_start_kernel completa o boot (dmesg vai ate o fim) mas nunca retorna.
 * Rodando numa thread real, thread_self() e sempre consistente.
 */
static void lkl_main_thread(void *arg)
{
	int ret;

	printf("lkl-switch: Linux Kernel Library no Horizon OS\n");
	printf("Inicializando LKL...\n");

	stage("B: antes de lkl_init");
	ret = lkl_init(&lkl_host_ops);
	printf("lkl_init() = %d\n", ret);
	stage(ret == 0 ? "C: lkl_init ok" : "C: lkl_init FALHOU");

	if (ret == 0) {
		printf("Bootando o kernel...\n\n");
		stage("D: chamando lkl_start_kernel");
		ret = lkl_start_kernel("mem=64M loglevel=8");
		stage("E: lkl_start_kernel retornou");
	}

	if (ret == 0) {
		/* fase 3: primeiro uso real do kernel via syscalls */
		/* wrapper de uname nao e gerado pelo LKL (guarda de macro
		 * errada em syscall_defs.h) — chamada crua */
		struct nx_utsname {
			char sysname[65], nodename[65], release[65],
			     version[65], machine[65], domainname[65];
		} un;
		long up[6] = { (long)&un, 0, 0, 0, 0, 0 };
		char buf[128] = {0};
		long r;
		int fd;

		printf("\n=== Linux vivo! ===\n");
		if (lkl_syscall(__lkl__NR_uname, up) == 0)
			printf("uname: %s %s %s\n",
			       un.sysname, un.release, un.machine);

		r = lkl_sys_mkdirat(LKL_AT_FDCWD, "/data", 0755);
		printf("mkdir /data = %ld\n", r);
		r = lkl_sys_mount("tmpfs", "/data", "tmpfs", 0, NULL);
		printf("mount tmpfs em /data = %ld\n", r);

		fd = lkl_sys_openat(LKL_AT_FDCWD, "/data/ola.txt",
				    LKL_O_RDWR | LKL_O_CREAT, 0644);
		if (fd >= 0) {
			const char msg[] = "ola do linux dentro do switch!";

			lkl_sys_write(fd, msg, sizeof(msg) - 1);
			lkl_sys_close(fd);
			fd = lkl_sys_openat(LKL_AT_FDCWD, "/data/ola.txt",
					    LKL_O_RDONLY, 0);
			r = lkl_sys_read(fd, buf, sizeof(buf) - 1);
			lkl_sys_close(fd);
			printf("lido de /data/ola.txt (%ld bytes): %s\n",
			       r, buf);
		} else {
			printf("openat falhou: %d\n", fd);
		}
		stage("F: demo de syscalls concluida");
		printf("\nlkl_start_kernel() = %d %s\n", ret,
		       ret == 0 ? "(kernel de pe!)" : "(falhou)");

		/* fase 8: executor de binarios ELF — busybox real */
		printf("\n=== Fase 8: busybox real ===\n");
		stage("G: antes do executor");
		lkl_sys_mkdirat(LKL_AT_FDCWD, "/tmp", 0755);
		lkl_sys_mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
		/* reserva fds 0/1/2 no LKL (senao o 1o open do guest pega fd 0=stdin) */
		for (int r = 0; r < 3; r++)
			lkl_sys_openat(LKL_AT_FDCWD, "/tmp/.stdio",
				       LKL_O_RDWR | LKL_O_CREAT, 0644);

		/* argv do busybox: "busybox ls /" */
		static char *bbargv[] = { "busybox", "ls", "/", NULL };
		nx_exec_argc = 3;
		nx_exec_argv = bbargv;

		printf("Rodando: busybox ls /\n\n");
		int xr = nx_exec_elf("sdmc:/busybox");   /* nao retorna (exit-syscall) */
		printf("nx_exec_elf = %d\n", xr);
		stage("H: executor retornou");
	}

	printf("\nAperte + para sair.\n");
}

int main(int argc, char *argv[])
{
	PadState pad;
	Thread disp, lkl;
	int have_lkl;

	consoleInit(NULL);
	remove("sdmc:/lkl-stage.log");
	stage("A: console ok");
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);

	/* thread de display: unica que chama consoleUpdate */
	if (R_SUCCEEDED(threadCreate(&disp, nx_display_thread, NULL, NULL,
				     0x8000, 0x2C, -2)))
		threadStart(&disp);

	/* todo o LKL numa thread real (ver comentario acima) */
	have_lkl = R_SUCCEEDED(threadCreate(&lkl, lkl_main_thread, NULL, NULL,
					    0x80000, 0x2C, -2));
	if (have_lkl)
		threadStart(&lkl);
	else
		stage("ERRO: threadCreate do LKL falhou");

	while (appletMainLoop()) {
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
			break;
		svcSleepThread(16666666ULL); /* ~60 Hz */
	}

	/* para as threads antes de fechar o console */
	display_stop = 1;
	threadWaitForExit(&disp);
	threadClose(&disp);
	if (have_lkl) {
		threadWaitForExit(&lkl);
		threadClose(&lkl);
	}
	consoleExit(NULL);
	return 0;
}
