/* s8: CPU life cycle.
 *  - IPIs sent to a stopped CPU are discarded;
 *  - a secondary can be stopped while it spins and restarted;
 *  - a machine reset (virt-ctrl) with all secondaries running stops
 *    them, and they can be started again after the reset.
 * A marker in RAM outside the image survives the reset. */
#include "smp.h"

#define MARKER      (*(volatile uint32_t *)0x00400000)
#define MAGIC       0x5e7b007u
#define CTRL_RESET  1

static volatile uint32_t pending_at_start[MAX_CPUS];
static volatile uint32_t spin[MAX_CPUS];

void secondary_main(int cpu)
{
    pending_at_start[cpu] = smp_read(SMP_IPI_PENDING) | 0x80000000u;
    for (;;) {
        spin[cpu]++;
    }
}

int main(void)
{
    int n;

    if (ncpus < 2) {
        printf("needs at least 2 cpus, skipping\n");
        return 0;
    }
    if (MARKER == MAGIC) {
        MARKER = 0;
        printf("after reset\n");
        for (n = 1; n < ncpus; n++) {
            CHECK(smp_read(SMP_CPU(n) + SMP_CPU_CTRL) == 0,
                  "cpu%d running after reset", n);
        }
        CHECK(smp_start_all() == ncpus, "restart after reset");
        return 0;
    }

    CHECK(smp_start_all() == ncpus, "start");
    n = ncpus - 1;
    {
        /* longer than the ~100 ms slice of single-threaded TCG */
        uint32_t s = spin[n];
        delay_ns(300000000);
        CHECK(spin[n] != s, "cpu%d not spinning", n);
    }
    smp_write(SMP_CPU(n) + SMP_CPU_CTRL, 0);
    delay_ns(1000000);
    {
        uint32_t s = spin[n];
        delay_ns(300000000);
        CHECK(spin[n] == s, "cpu%d still running after stop", n);
    }
    ipi_send(1u << n, 0xffff);      /* discarded */
    pending_at_start[n] = 0;
    cpu_online[n] = 0;
    CHECK(smp_start_cpu(n) == 0, "restart cpu%d", n);
    while (!pending_at_start[n]) {
    }
    CHECK(pending_at_start[n] == 0x80000000u, "IPI kept for stopped cpu: %08lx",
          (unsigned long)pending_at_start[n]);

    printf("resetting with %d cpus spinning\n", ncpus);
    MARKER = MAGIC;
    mmio_write32(bootinfo.ctrl_base + 4, CTRL_RESET);
    panic("reset did not happen");
}
