/* s7: routing of the goldfish-pic levels with DEVIRQ_ROUTE.  The RTC
 * alarm (PIC #6 -> IPL 6) is routed to each CPU in turn; only that CPU
 * may take the interrupt. */
#include "smp.h"

static uintptr_t timer;
static volatile uint32_t taken[MAX_CPUS];
static volatile uint32_t quit;

static void rtc_handler(struct exc_frame *f, uint32_t *regs)
{
    mmio_write32(timer + RTC_CLEAR_INTERRUPT, 1);
    taken[cpu_id()]++;
}

void secondary_main(int cpu)
{
    set_ipl(0);
    for (;;) {
        cpu_stop(0);
    }
}

int main(void)
{
    uint32_t timer_irq;

    if (!bootinfo.smp_base) {
        printf("no virt-smp, skipping\n");
        return 0;
    }
    timer = bootinfo.rtc_base + 0x1000;
    timer_irq = bootinfo.rtc_irq + 1;
    set_handler(24 + 6, rtc_handler);
    CHECK(smp_read(SMP_DEVIRQ_ROUTE) == 0, "route reset value");
    if (smp_start_all() != ncpus) {
        panic("cpus did not start");
    }
    pic_enable(timer_irq);
    mmio_write32(timer + RTC_IRQ_ENABLED, 1);
    set_ipl(0);

    for (int k = 0; k < ncpus; k++) {
        uint64_t t;

        smp_write(SMP_DEVIRQ_ROUTE, k << (4 * (6 - 1)));
        rtc_set_alarm(timer, rtc_ns(timer) + 1000000);
        t = rtc_ns(bootinfo.rtc_base);
        while (taken[k] == 0 && rtc_ns(bootinfo.rtc_base) - t < 2000000000ull) {
        }
        for (int n = 0; n < ncpus; n++) {
            CHECK(taken[n] == (n == k), "route to cpu%d: cpu%d took %lu", k, n,
                  (unsigned long)taken[n]);
            taken[n] = 0;
        }
    }
    /* out-of-range targets fall back to cpu0 */
    smp_write(SMP_DEVIRQ_ROUTE, 15 << (4 * (6 - 1)));
    if (ncpus < 16) {
        rtc_set_alarm(timer, rtc_ns(timer) + 1000000);
        delay_ns(20000000);
        CHECK(taken[0] == 1, "fallback route: cpu0 took %lu",
              (unsigned long)taken[0]);
    }
    set_ipl(7);
    mmio_write32(timer + RTC_IRQ_ENABLED, 0);
    printf("routed the RTC interrupt to each of %d cpus\n", ncpus);
    return 0;
}
