/* virt-smp helpers for the bare-metal tests. */
#include "smp.h"

#define STACK_SIZE 16384

int ncpus = 1;
volatile uint32_t cpu_online[MAX_CPUS];

uint64_t smp_counter_ns(void)
{
    uint32_t lo = smp_read(SMP_COUNTER_LO);
    uint32_t hi = smp_read(SMP_COUNTER_HI);
    return ((uint64_t)hi << 32) | lo;
}

int smp_start_cpu(int n)
{
    uint64_t t;

    smp_write(SMP_CPU(n) + SMP_START_PC, (uint32_t)_secondary_start);
    smp_write(SMP_CPU(n) + SMP_START_SP,
              (uint32_t)_stacks + (n + 1) * STACK_SIZE);
    smp_write(SMP_CPU(n) + SMP_START_ARG, n);
    smp_write(SMP_CPU(n) + SMP_CPU_CTRL, 1);
    t = rtc_ns(bootinfo.rtc_base);
    while (!cpu_online[n]) {
        if (rtc_ns(bootinfo.rtc_base) - t > 2000000000ull) {
            printf("cpu%d did not come online\n", n);
            return -1;
        }
    }
    return 0;
}

int smp_start_all(void)
{
    int n, up = 1;

    for (n = 1; n < ncpus; n++) {
        if (smp_start_cpu(n) == 0) {
            up++;
        }
    }
    return up;
}

/* Sense-reversing barrier across all CPUs */
static volatile uint32_t bar_count, bar_sense;

void smp_barrier(void)
{
    uint32_t sense = bar_sense;

    if (atomic_add_return(&bar_count, 1) == (uint32_t)ncpus) {
        bar_count = 0;
        bar_sense = !sense;
    } else {
        while (bar_sense == sense) {
        }
    }
}

/* Called from cpu_main(); see lib.c */
void smp_init_boot(void)
{
    if (bootinfo.smp_base) {
        ncpus = smp_read(SMP_NCPUS);
    }
    cpu_online[0] = 1;
}

void smp_init_secondary(int cpu)
{
    cpu_online[cpu] = 1;
}
