/*
 * dl - dlopen() of a library the program is not linked against, a call into
 * it, and a file written to /tmp then read back.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	failures++;
}

int main(void)
{
	static const char path[] = "/tmp/gx-dl-test.txt";
	static const char text[] = "written through LKL\n";
	double (*cosine)(double);
	char buf[128] = { 0 };
	void *lib;
	FILE *f;

	lib = dlopen("libm.so.6", RTLD_NOW);
	if (!lib) {
		printf("dlopen: %s\n", dlerror());
		fail("dlopen libm.so.6");
	} else {
		cosine = (double (*)(double))dlsym(lib, "cos");
		if (!cosine) {
			printf("dlsym: %s\n", dlerror());
			fail("dlsym cos");
		} else {
			volatile double zero = 0.0, pi = 3.14159265358979323846;
			double a = cosine(zero), b = cosine(pi);

			printf("cos(0) = %f, cos(pi) = %f\n", a, b);
			if (a != 1.0 || b > -0.999999 || b < -1.000001)
				fail("cos result");
		}
		if (dlclose(lib))
			fail("dlclose");
	}

	f = fopen(path, "w");
	if (!f) {
		fail("fopen for writing");
	} else {
		if (fputs(text, f) < 0)
			fail("fputs");
		if (fclose(f))
			fail("fclose");
	}
	f = fopen(path, "r");
	if (!f) {
		fail("fopen for reading");
	} else {
		if (!fgets(buf, sizeof(buf), f) || strcmp(buf, text))
			fail("read back");
		fclose(f);
	}
	if (unlink(path))
		fail("unlink");
	if (access(path, F_OK) == 0)
		fail("the file survived unlink");

	printf("%s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
