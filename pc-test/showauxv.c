/* imprime o auxv que o host passou — pra comparar qemu-user vs nosso executor */
#include <stdio.h>
extern char **environ;
int main(int argc, char **argv)
{
	char **e = environ;
	while (*e) e++;
	e++;                        /* pula o NULL do fim de envp */
	unsigned long *a = (unsigned long *)e;
	while (a[0]) {
		printf("AT %lu = 0x%lx\n", a[0], a[1]);
		a += 2;
	}
	return 0;
}
