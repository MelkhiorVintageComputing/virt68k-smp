/*
 * pmap micro-benchmarks for NetBSD MP (run several copies in parallel
 * with pmap-bench.sh).
 *
 * usage: pmap-bench fork N       N x fork() + _exit() + wait()
 *        pmap-bench exec N       N x vfork() + exec /usr/bin/true
 *        pmap-bench mmap N MB    N x mmap MB anonymous, touch, munmap
 *        pmap-bench mprot N      2 threads: one mprotect()s a page RO/RW
 *                                N times while the other reads it
 *        pmap-bench fault N MB   touch MB of fresh memory, N times, with
 *                                4 threads of one process
 */
#include <sys/mman.h>
#include <sys/wait.h>
#include <err.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

static long pgsz;

static void
do_fork(int n)
{
	for (int i = 0; i < n; i++) {
		pid_t p = fork();
		if (p < 0)
			err(1, "fork");
		if (p == 0)
			_exit(0);
		waitpid(p, NULL, 0);
	}
}

static void
do_exec(int n)
{
	for (int i = 0; i < n; i++) {
		pid_t p = vfork();
		if (p < 0)
			err(1, "vfork");
		if (p == 0) {
			execl("/usr/bin/true", "true", (char *)NULL);
			_exit(127);
		}
		waitpid(p, NULL, 0);
	}
}

static void
touch(char *p, size_t len)
{
	for (size_t o = 0; o < len; o += pgsz)
		p[o] = 1;
}

static void
do_mmap(int n, size_t mb)
{
	size_t len = mb << 20;

	for (int i = 0; i < n; i++) {
		char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		    MAP_ANON | MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED)
			err(1, "mmap");
		touch(p, len);
		munmap(p, len);
	}
}

static volatile int mprot_done;
static volatile char *mprot_page;

static void *
mprot_reader(void *arg)
{
	unsigned sum = 0;

	while (!mprot_done)
		sum += mprot_page[0];
	return (void *)(uintptr_t)sum;
}

static void
do_mprot(int n)
{
	pthread_t t;
	char *p = mmap(NULL, pgsz, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);

	if (p == MAP_FAILED)
		err(1, "mmap");
	p[0] = 1;
	mprot_page = p;
	pthread_create(&t, NULL, mprot_reader, NULL);
	for (int i = 0; i < n; i++) {
		mprotect(p, pgsz, PROT_READ);
		mprotect(p, pgsz, PROT_READ | PROT_WRITE);
	}
	mprot_done = 1;
	pthread_join(t, NULL);
}

struct fault_arg {
	int n;
	size_t len;
};

static void *
fault_thread(void *arg)
{
	struct fault_arg *fa = arg;

	do_mmap(fa->n, fa->len >> 20);
	return NULL;
}

static void
do_fault(int n, size_t mb)
{
	pthread_t t[4];
	struct fault_arg fa = { n, mb << 20 };

	for (int i = 0; i < 4; i++)
		pthread_create(&t[i], NULL, fault_thread, &fa);
	for (int i = 0; i < 4; i++)
		pthread_join(t[i], NULL);
}

int
main(int argc, char **argv)
{
	pgsz = sysconf(_SC_PAGESIZE);
	if (argc < 3)
		errx(2, "usage: pmap-bench fork|exec|mmap|mprot|fault N [MB]");
	int n = atoi(argv[2]);
	size_t mb = argc > 3 ? (size_t)atoi(argv[3]) : 16;

	if (strcmp(argv[1], "fork") == 0)
		do_fork(n);
	else if (strcmp(argv[1], "exec") == 0)
		do_exec(n);
	else if (strcmp(argv[1], "mmap") == 0)
		do_mmap(n, mb);
	else if (strcmp(argv[1], "mprot") == 0)
		do_mprot(n);
	else if (strcmp(argv[1], "fault") == 0)
		do_fault(n, mb);
	else
		errx(2, "unknown test %s", argv[1]);
	return 0;
}
