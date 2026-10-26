/*
 * pmap_unwire stress: threads keep dirtying a MAP_SHARED file mapping
 * while the main thread mlock()s / munlock()s it; then msync, check the
 * file contents, and check that the wired page count is back to where it
 * started.  usage: wiretest [threads] [rounds]
 */
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <uvm/uvm_extern.h>
#include <err.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define	NPG	64
static long pgsz;
static volatile unsigned *map;
static volatile int stop;
static int nthr = 7;

static long
wired(void)
{
	struct uvmexp_sysctl u;
	size_t len = sizeof(u);
	int mib[2] = { CTL_VM, VM_UVMEXP2 };

	if (sysctl(mib, 2, &u, &len, NULL, 0) == -1)
		err(1, "sysctl");
	return (long)u.wired;
}

static void *
writer(void *arg)
{
	long id = (long)arg;
	unsigned gen = 0;
	size_t words = NPG * pgsz / sizeof(unsigned);

	while (!stop) {
		gen++;
		/* each thread owns the words i with i % nthr == id */
		for (size_t i = id; i < words; i += nthr)
			map[i] = (unsigned)(id << 24) | (gen & 0xffffff);
	}
	/* final, known value */
	for (size_t i = id; i < words; i += nthr)
		map[i] = (unsigned)(id << 24) | 0xabcdef;
	return NULL;
}

int
main(int argc, char **argv)
{
	int rounds = argc > 2 ? atoi(argv[2]) : 2000;
	pthread_t t[16];
	long w0, w1;
	char path[] = "/tmp/wiretest.XXXXXX";
	int fd;

	if (argc > 1)
		nthr = atoi(argv[1]);
	pgsz = sysconf(_SC_PAGESIZE);
	if ((fd = mkstemp(path)) == -1 || ftruncate(fd, NPG * pgsz) == -1)
		err(1, "file");
	map = mmap(NULL, NPG * pgsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "mmap");
	w0 = wired();
	for (long i = 0; i < nthr; i++)
		pthread_create(&t[i], NULL, writer, (void *)i);
	for (int r = 0; r < rounds; r++) {
		if (mlock((void *)map, NPG * pgsz) == -1)
			err(1, "mlock");
		if (munlock((void *)map, NPG * pgsz) == -1)
			err(1, "munlock");
	}
	stop = 1;
	for (int i = 0; i < nthr; i++)
		pthread_join(t[i], NULL);
	if (msync((void *)map, NPG * pgsz, MS_SYNC) == -1)
		err(1, "msync");
	munmap((void *)map, NPG * pgsz);

	/* the file must hold the final values */
	size_t words = NPG * pgsz / sizeof(unsigned);
	unsigned *buf = malloc(NPG * pgsz);
	long bad = 0;
	if (pread(fd, buf, NPG * pgsz, 0) != NPG * pgsz)
		err(1, "pread");
	for (size_t i = 0; i < words; i++)
		if (buf[i] != ((unsigned)((i % nthr) << 24) | 0xabcdef))
			bad++;
	close(fd);
	unlink(path);
	w1 = wired();
	printf("wiretest: %d rounds, %d threads, %ld bad words, wired %ld -> %ld\n",
	    rounds, nthr, bad, w0, w1);
	printf("wiretest %s\n", bad == 0 && w1 <= w0 + 2 ? "PASS" : "FAIL");
	return bad != 0;
}
