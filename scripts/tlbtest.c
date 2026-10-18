/*
 * TLB coherence test for NetBSD MP: checks that a change of a mapping by
 * one thread is seen by the threads of the same process running on the
 * other CPUs, which keep the old translation hot in their TLB (they
 * access the page in a loop until the change is made).
 *
 *   ro     after mprotect(PROT_READ), a write must fault
 *   unmap  after munmap(), a read must fault
 *   remap  after mmap(MAP_FIXED) of a new page, a read must see the new data
 *   cow    after fork(), the writes of a thread must not reach the child
 *
 * usage: tlbtest [-t THREADS] [-r ROUNDS] [test...]   (default: all tests)
 * Each reader thread is bound to its own CPU when possible.  Prints
 * "tlbtest PASS" or "tlbtest FAIL" and exits 0 or 1.
 */
#include <sys/mman.h>
#include <sys/wait.h>
#include <sched.h>
#include <err.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	MAXT	16

static long pgsz;
static int nthreads = 3, rounds = 2000;
static char * volatile page;		/* the page under test */

/* round protocol */
static volatile int round_no;		/* readers start round round_no */
static volatile int changed;		/* == round_no once the change is done */
static volatile int ready, done;	/* readers counts for this round */
static volatile int test_id;
static volatile int quit;
static volatile long failures;
static pthread_mutex_t cnt_mtx = PTHREAD_MUTEX_INITIALIZER;

enum { T_RO, T_UNMAP, T_REMAP, T_COW, T_N };
static const char *tname[T_N] = { "ro", "unmap", "remap", "cow" };

static __thread sigjmp_buf jb;
static __thread volatile int in_probe;

static void
segv(int sig)
{
	if (in_probe)
		siglongjmp(jb, 1);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
inc(volatile int *p)
{
	pthread_mutex_lock(&cnt_mtx);
	(*p)++;
	pthread_mutex_unlock(&cnt_mtx);
}

static void
fail(const char *what, int r, long v)
{
	pthread_mutex_lock(&cnt_mtx);
	failures++;
	if (failures < 10)
		printf("FAIL %s round %d (%ld)\n", what, r, v);
	pthread_mutex_unlock(&cnt_mtx);
}

/* access page[0]; returns -1 if it faulted, else the value read */
static long
probe(int write, int val)
{
	volatile unsigned char *p = (volatile unsigned char *)page;
	long v;

	if (sigsetjmp(jb, 1)) {
		in_probe = 0;
		return -1;
	}
	in_probe = 1;
	if (write) {
		p[0] = val;
		v = val;
	} else
		v = p[0];
	in_probe = 0;
	return v;
}

static void
bind_cpu(int n)
{
	cpuset_t *cs = cpuset_create();

	if (cs == NULL)
		return;
	cpuset_zero(cs);
	cpuset_set(n % sysconf(_SC_NPROCESSORS_ONLN), cs);
	(void)pthread_setaffinity_np(pthread_self(), cpuset_size(cs), cs);
	cpuset_destroy(cs);
}

static void *
reader(void *arg)
{
	int id = (int)(long)arg, r = 0, k = 0;
	long v;

	bind_cpu(id + 1);
	for (;;) {
		while (round_no == r && !quit)
			continue;
		if (quit)
			return NULL;
		r = round_no;
		/* warm the TLB with the old mapping until the change */
		inc(&ready);
		while (changed != r) {
			switch (test_id) {
			case T_RO:
				(void)probe(1, r & 0x7f);
				break;
			case T_UNMAP:
			case T_REMAP:
				(void)probe(0, 0);
				break;
			case T_COW:
				(void)probe(1, ++k & 0x7f);	/* keep writing */
				break;
			}
		}
		switch (test_id) {
		case T_RO:
			if ((v = probe(1, 0x55)) != -1)
				fail("ro: write did not fault", r, v);
			break;
		case T_UNMAP:
			if ((v = probe(0, 0)) != -1)
				fail("unmap: read did not fault", r, v);
			break;
		case T_REMAP:
			if ((v = probe(0, 0)) != (r & 0x7f) + 1)
				fail("remap: stale data", r, v);
			break;
		case T_COW:
			/* writes go on while the child checks its copy */
			for (int j = 0; j < 2000; j++)
				(void)probe(1, ++k & 0x7f);
			break;
		}
		inc(&done);
	}
}

static void
wait_count(volatile int *c, int n)
{
	while (*c < n)
		sched_yield();
}

static void
run_test(int t)
{
	long before = failures;

	test_id = t;
	for (int r = 1; r <= rounds; r++) {
		/* set up the page for this round */
		page = mmap(page, pgsz, PROT_READ | PROT_WRITE,
		    MAP_ANON | MAP_PRIVATE | (page ? MAP_FIXED : 0), -1, 0);
		if (page == MAP_FAILED)
			err(1, "mmap");
		page[0] = r & 0x7f;
		ready = done = 0;
		round_no = round_no + 1;
		wait_count(&ready, nthreads);
		switch (t) {
		case T_RO:
			mprotect(page, pgsz, PROT_READ);
			break;
		case T_UNMAP:
			munmap(page, pgsz);
			break;
		case T_REMAP: {
			char *n = mmap(page, pgsz, PROT_READ | PROT_WRITE,
			    MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
			if (n != page)
				err(1, "mmap fixed");
			n[0] = (round_no & 0x7f) + 1;
			break;
		}
		case T_COW: {
			pid_t p = fork();
			if (p == 0) {
				/* the child's copy must not change */
				char v0 = page[0];
				for (int k = 0; k < 20000; k++)
					if (page[0] != v0)
						_exit(1);
				_exit(0);
			}
			int st;
			changed = round_no;
			waitpid(p, &st, 0);
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
				fail("cow: parent write reached the child", r,
				    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
			break;
		}
		}
		changed = round_no;
		wait_count(&done, nthreads);
		if (t == T_RO)
			mprotect(page, pgsz, PROT_READ | PROT_WRITE);
		if (t == T_UNMAP)
			page = NULL;
	}
	printf("%-6s %d rounds, %ld failures\n", tname[t], rounds,
	    failures - before);
}

int
main(int argc, char **argv)
{
	pthread_t th[MAXT];
	int ch, sel[T_N] = { 0 }, any = 0;

	while ((ch = getopt(argc, argv, "t:r:")) != -1) {
		switch (ch) {
		case 't':
			nthreads = atoi(optarg);
			break;
		case 'r':
			rounds = atoi(optarg);
			break;
		default:
			errx(2, "usage: tlbtest [-t threads] [-r rounds] [test...]");
		}
	}
	if (nthreads < 1 || nthreads > MAXT)
		errx(2, "1 to %d threads", MAXT);
	for (int i = optind; i < argc; i++) {
		int t;
		for (t = 0; t < T_N; t++)
			if (strcmp(argv[i], tname[t]) == 0)
				break;
		if (t == T_N)
			errx(2, "unknown test %s", argv[i]);
		sel[t] = any = 1;
	}
	pgsz = sysconf(_SC_PAGESIZE);
	signal(SIGSEGV, segv);
	signal(SIGBUS, segv);
	setvbuf(stdout, NULL, _IOLBF, 0);
	bind_cpu(0);
	for (int i = 0; i < nthreads; i++)
		pthread_create(&th[i], NULL, reader, (void *)(long)i);
	for (int t = 0; t < T_N; t++)
		if (!any || sel[t])
			run_test(t);
	quit = 1;
	for (int i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	printf("tlbtest %s\n", failures ? "FAIL" : "PASS");
	return failures != 0;
}
