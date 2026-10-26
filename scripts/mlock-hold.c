/*
 * Wire NLOCK pages (mlock) and keep NFREE more pages mapped but not
 * wired, then stay on the CPU touching them until killed, so that this
 * process's page tables are those of the current user root pointer.
 * usage: mlock-hold NLOCK NFREE
 */
#include <sys/mman.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	long pg = sysconf(_SC_PAGESIZE);
	long nl = argc > 1 ? atol(argv[1]) : 64;
	long nf = argc > 2 ? atol(argv[2]) : 64;
	volatile char *l, *f;

	l = mmap(NULL, nl * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	f = mmap(NULL, nf * pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	if (l == MAP_FAILED || f == MAP_FAILED)
		err(1, "mmap");
	if (mlock((void *)l, nl * pg) == -1)
		err(1, "mlock");
	printf("locked %ld pages at %p, %ld unlocked pages at %p\n",
	    nl, (void *)l, nf, (void *)f);
	fflush(stdout);
	for (;;) {
		for (long i = 0; i < nl; i++)
			l[i * pg]++;
		for (long i = 0; i < nf; i++)
			f[i * pg]++;
	}
}
