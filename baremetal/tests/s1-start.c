/* s1: start every secondary CPU; each checks its ID and prints under
 * the console lock.  Also checks stop/restart of a secondary. */
#include "smp.h"

static volatile uint32_t ids[MAX_CPUS];
static volatile uint32_t arg_d0[MAX_CPUS], arg_a0[MAX_CPUS];
static volatile uint32_t generation[MAX_CPUS];
static volatile uint32_t done;

void secondary_main(int cpu)
{
    ids[cpu] = cpu_id();
    generation[cpu]++;
    printf("  hello from cpu%d (id %lu, sr %04x, sp %p)\n", cpu,
           (unsigned long)ids[cpu], get_sr(), __builtin_frame_address(0));
    atomic_inc(&done);
}

int main(void)
{
    int n, up;

    printf("ncpus=%d version=%lu\n", ncpus,
           bootinfo.smp_base ? (unsigned long)smp_read(SMP_VERSION) : 0ul);
    CHECK(cpu_id() == 0, "boot cpu id %d", cpu_id());
    if (ncpus == 1) {
        CHECK(bootinfo.smp_base == 0, "virt-smp present with 1 cpu");
        return 0;
    }
    CHECK(bootinfo.smp_irq == (uint32_t)ncpus, "bootinfo ncpus %lu",
          (unsigned long)bootinfo.smp_irq);
    for (n = 1; n < ncpus; n++) {
        CHECK(smp_read(SMP_CPU(n) + SMP_CPU_CTRL) == 0, "cpu%d running", n);
    }
    CHECK(smp_read(SMP_CPU(0) + SMP_CPU_CTRL) == 1, "cpu0 not running");

    up = smp_start_all();
    CHECK(up == ncpus, "%d of %d cpus online", up, ncpus);
    while (done != (uint32_t)up - 1) {
    }
    for (n = 1; n < ncpus; n++) {
        CHECK(ids[n] == (uint32_t)n, "cpu%d has id %lu", n,
              (unsigned long)ids[n]);
        CHECK(smp_read(SMP_CPU(n) + SMP_CPU_CTRL) == 1, "cpu%d not running", n);
    }

    /* stop the last CPU, restart it: it runs secondary_main again */
    n = ncpus - 1;
    smp_write(SMP_CPU(n) + SMP_CPU_CTRL, 0);
    CHECK(smp_read(SMP_CPU(n) + SMP_CPU_CTRL) == 0, "cpu%d did not stop", n);
    /* cpu0 cannot be stopped */
    smp_write(SMP_CPU(0) + SMP_CPU_CTRL, 0);
    CHECK(smp_read(SMP_CPU(0) + SMP_CPU_CTRL) == 1, "cpu0 stopped");
    cpu_online[n] = 0;
    CHECK(smp_start_cpu(n) == 0, "restart of cpu%d", n);
    while (done != (uint32_t)up) {
    }
    CHECK(generation[n] == 2, "cpu%d generation %lu", n,
          (unsigned long)generation[n]);
    return 0;
}
