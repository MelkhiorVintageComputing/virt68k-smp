/* s2: all CPUs hammer shared counters with CAS, a TAS spinlock and
 * CAS2 (adjacent and non-adjacent operands).  The atomic counters must
 * be exact.  A deliberately racy load/add/store counter shows whether the
 * vCPUs really run in parallel (it loses updates only with MTTCG). */
#include "smp.h"

#define ITER 100000

static volatile uint32_t cas_counter, locked_counter, racy_counter;
static volatile uint32_t lock_owner = ~0u;
static volatile uint32_t lock_violations;
static spinlock_t lock;
static volatile uint32_t pair[2];                  /* adjacent: (x, 2x) */
static volatile uint32_t sep_a, pad[64], sep_b;    /* separate: (x, 3x) */
static volatile uint32_t finished;
static volatile uint32_t compute_go, compute_done;
static volatile uint32_t sink[MAX_CPUS * 16];

/* Independent compute: no shared cache lines, no atomics. */
static void compute(int cpu)
{
    uint32_t x = cpu + 1;
    for (int i = 0; i < 20000000; i++) {
        x = x * 1103515245u + 12345u;
    }
    sink[cpu * 16] = x;
}

static void cas2_add(volatile uint32_t *a, volatile uint32_t *b, uint32_t k)
{
    register uint32_t c1 __asm__("d0") = *a;
    register uint32_t c2 __asm__("d1") = *b;
    register uint32_t u1 __asm__("d2");
    register uint32_t u2 __asm__("d3");
    register volatile uint32_t *a1 __asm__("a0") = a;
    register volatile uint32_t *a2 __asm__("a1") = b;
    uint16_t ccr;

    do {
        u1 = c1 + 1;
        u2 = c2 + k;
        __asm__ volatile("cas2l %%d0:%%d1,%%d2:%%d3,(%%a0):(%%a1); movew %%ccr,%0"
                         : "=d"(ccr), "+d"(c1), "+d"(c2)
                         : "d"(u1), "d"(u2), "a"(a1), "a"(a2)
                         : "cc", "memory");
    } while (!(ccr & 4));
}

static void work(int cpu)
{
    for (int i = 0; i < ITER; i++) {
        uint32_t v;

        atomic_inc(&cas_counter);

        spin_lock(&lock);
        if (lock_owner != ~0u) {
            lock_violations++;
        }
        lock_owner = cpu;
        locked_counter++;
        lock_owner = ~0u;
        spin_unlock(&lock);

        v = racy_counter;
        __asm__ volatile("" : "+d"(v));
        racy_counter = v + 1;

        if ((i & 7) == 0) {
            cas2_add(&pair[0], &pair[1], 2);
            cas2_add(&sep_a, &sep_b, 3);
        }
    }
}

void secondary_main(int cpu)
{
    while (!compute_go) {
    }
    compute(cpu);
    atomic_inc(&compute_done);
    smp_barrier();
    work(cpu);
    atomic_inc(&finished);
}

int main(void)
{
    uint64_t t0, t1;
    uint32_t expect = ncpus * ITER, expect2 = ncpus * ((ITER + 7) / 8);

    uint64_t t_one, t_all;

    if (smp_start_all() != ncpus) {
        panic("cpus did not start");
    }
    /* scaling: one CPU alone, then all CPUs at once */
    t0 = rtc_ns(bootinfo.rtc_base);
    compute(0);
    t_one = rtc_ns(bootinfo.rtc_base) - t0;
    t0 = rtc_ns(bootinfo.rtc_base);
    compute_go = 1;
    compute(0);
    while (compute_done != (uint32_t)ncpus - 1) {
    }
    t_all = rtc_ns(bootinfo.rtc_base) - t0;
    printf("compute: 1 cpu %lu ms, %d cpus %lu ms, SPEEDUP %lu.%02lu\n",
           (unsigned long)(t_one / 1000000), ncpus,
           (unsigned long)(t_all / 1000000),
           (unsigned long)(ncpus * t_one / t_all),
           (unsigned long)(ncpus * t_one * 100 / t_all) % 100);

    t0 = rtc_ns(bootinfo.rtc_base);
    smp_barrier();
    work(0);
    while (finished != (uint32_t)ncpus - 1) {
    }
    t1 = rtc_ns(bootinfo.rtc_base);

    printf("%d cpus x %d iterations in %lu ms\n", ncpus, ITER,
           (unsigned long)((t1 - t0) / 1000000));
    printf("cas %lu locked %lu racy %lu (lost %lu) pair %lu/%lu sep %lu/%lu\n",
           (unsigned long)cas_counter, (unsigned long)locked_counter,
           (unsigned long)racy_counter,
           (unsigned long)(expect - racy_counter),
           (unsigned long)pair[0], (unsigned long)pair[1],
           (unsigned long)sep_a, (unsigned long)sep_b);
    CHECK(cas_counter == expect, "CAS counter");
    CHECK(locked_counter == expect, "TAS-locked counter");
    CHECK(lock_violations == 0, "%lu mutual exclusion violations",
          (unsigned long)lock_violations);
    CHECK(pair[0] == expect2 && pair[1] == 2 * expect2, "adjacent CAS2");
    CHECK(sep_a == expect2 && sep_b == 3 * expect2, "separate CAS2");
    if (ncpus > 1) {
        printf("PARALLEL: %s\n", racy_counter != expect ? "yes" : "not observed");
    }
    (void)pad;
    return 0;
}
