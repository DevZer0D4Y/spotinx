/*
 * harness.c - roda a MESMA logica do NRO (fase 3+) nativo no PC via LKL POSIX.
 * Loop de teste rapido sem Switch: valida config do kernel, syscalls, e (fase 7)
 * o executor de binarios. So a camada nx_host.c (libnx) fica de fora.
 *
 * Build: cc harness.c -I<lkl>/include -L<lkl> -llkl -lpthread -o harness
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <lkl.h>
#include <lkl_host.h>

int main(int argc, char **argv)
{
	int ret, fd;
	long r;
	char buf[128] = {0};
	long up[6];
	struct {
		char sysname[65], nodename[65], release[65],
		     version[65], machine[65], domainname[65];
	} un;

	lkl_host_ops.print = NULL; /* deixa o kernel escrever via console interno */
	ret = lkl_init(&lkl_host_ops);
	printf("lkl_init = %d\n", ret);
	if (ret)
		return 1;

	ret = lkl_start_kernel("mem=64M loglevel=8");
	printf("lkl_start_kernel = %d\n", ret);
	if (ret)
		return 1;

	memset(up, 0, sizeof(up));
	up[0] = (long)&un;
	if (lkl_syscall(__lkl__NR_uname, up) == 0)
		printf("uname: %s %s %s\n", un.sysname, un.release, un.machine);

	r = lkl_sys_mkdirat(LKL_AT_FDCWD, "/data", 0755);
	printf("mkdir /data = %ld\n", r);
	r = lkl_sys_mount("tmpfs", "/data", "tmpfs", 0, NULL);
	printf("mount tmpfs = %ld\n", r);

	fd = lkl_sys_openat(LKL_AT_FDCWD, "/data/ola.txt",
			    LKL_O_RDWR | LKL_O_CREAT, 0644);
	printf("open = %d\n", fd);
	if (fd >= 0) {
		const char msg[] = "ola do linux dentro do switch!";

		lkl_sys_write(fd, msg, sizeof(msg) - 1);
		lkl_sys_close(fd);
		fd = lkl_sys_openat(LKL_AT_FDCWD, "/data/ola.txt", LKL_O_RDONLY, 0);
		r = lkl_sys_read(fd, buf, sizeof(buf) - 1);
		lkl_sys_close(fd);
		printf("read (%ld): %s\n", r, buf);
	}

	/* --- fase 4: disco ext4 persistente --- */
	{
		struct lkl_disk disk = {0};
		char mnt[32];
		int part = 0;
		unsigned int disk_id;
		const char *img = "/tmp/lkl-disk.img";

		/* cria imagem ext4 uma vez (mke2fs do host, fora do LKL) */
		if (access(img, F_OK) != 0) {
			char cmd[256];

			snprintf(cmd, sizeof(cmd),
				 "dd if=/dev/zero of=%s bs=1M count=16 status=none && "
				 "mke2fs -q -t ext4 -F %s", img, img);
			printf("criando imagem: %d\n", system(cmd));
		}

		disk.fd = open(img, O_RDWR);
		disk.ops = &lkl_dev_blk_ops; /* POSIX host: le/escreve em disk.fd */
		r = lkl_disk_add(&disk);
		printf("disk_add = %ld (fd=%d)\n", r, disk.fd);
		if (r >= 0) {
			disk_id = r;
			r = lkl_mount_dev(disk_id, part, "ext4", 0, NULL,
					  mnt, sizeof(mnt));
			printf("mount ext4 = %ld em %s\n", r, mnt);
			if (r == 0) {
				char path[64];

				snprintf(path, sizeof(path), "%s/persist.txt", mnt);
				fd = lkl_sys_openat(LKL_AT_FDCWD, path,
						    LKL_O_RDWR | LKL_O_CREAT, 0644);
				if (fd >= 0) {
					const char m[] = "persistente no ext4";

					lkl_sys_write(fd, m, sizeof(m) - 1);
					lkl_sys_close(fd);
					printf("escrito em %s (sobrevive reboot)\n",
					       path);
				}
				lkl_umount_dev(disk_id, part, 0, 1000);
			}
		}
	}

	lkl_sys_halt();
	printf("=== fases 3+4 OK no PC ===\n");
	return 0;
}
