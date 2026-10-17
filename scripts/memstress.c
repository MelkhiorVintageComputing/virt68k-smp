/* Paging stress for NetBSD MP testing: each process allocates MB megabytes,
 * fills them with a pattern derived from its pid and the round, and
 * verifies them; with several processes and total > RAM this pages
 * heavily.  usage: memstress MB ROUNDS */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	size_t mb = argc > 1 ? atoi(argv[1]) : 100;
	int rounds = argc > 2 ? atoi(argv[2]) : 5;
	size_t n = mb * 1024 * 1024 / sizeof(unsigned);
	unsigned *p = malloc(n * sizeof(unsigned));
	unsigned seed = getpid() * 2654435761u;

	if (p == NULL) {
		perror("malloc");
		return 2;
	}
	for (int r = 0; r < rounds; r++) {
		unsigned k = seed + r;
		for (size_t i = 0; i < n; i += 1024 / sizeof(unsigned))
			p[i] = k ^ (unsigned)i;
		for (size_t i = 0; i < n; i += 1024 / sizeof(unsigned))
			if (p[i] != (k ^ (unsigned)i)) {
				printf("pid %d round %d: CORRUPT at %zu: %08x != %08x\n",
				    getpid(), r, i, p[i], k ^ (unsigned)i);
				return 1;
			}
	}
	printf("pid %d ok\n", getpid());
	return 0;
}
