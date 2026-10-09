/* hello - the smallest dynamically linked guest: printf and malloc. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	size_t total = 0;
	int ok = 1;

	printf("hello from %s (%d argument%s)\n", argv[0], argc - 1, argc == 2 ? "" : "s");

	/* Small blocks come from brk, the large one from mmap. */
	for (int i = 0; i < 1000; i++) {
		size_t len = 16 + (i * 37) % 4000;
		unsigned char *p = malloc(len);

		if (!p) {
			ok = 0;
			break;
		}
		memset(p, i & 0xff, len);
		if (p[0] != (i & 0xff) || p[len - 1] != (i & 0xff))
			ok = 0;
		total += len;
		if (i % 3)
			free(p);
	}
	{
		size_t len = 8u << 20;
		unsigned char *big = malloc(len);

		if (!big) {
			ok = 0;
		} else {
			memset(big, 0x5a, len);
			if (big[0] != 0x5a || big[len - 1] != 0x5a)
				ok = 0;
			free(big);
		}
	}
	printf("malloc: %zu bytes in small blocks, pi is about %.5f\n", total, 3.14159265);
	printf("processors: %ld online, %ld configured\n", sysconf(_SC_NPROCESSORS_ONLN),
	       sysconf(_SC_NPROCESSORS_CONF));
	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
