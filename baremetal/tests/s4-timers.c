/* s4: per-CPU virt-smp timers.  Every CPU runs a periodic timer at its
 * own rate and counts ticks while idling in STOP; the counts must
 * match the elapsed time.  Also checks one-shot mode, the global
 * counter, and that timer interrupts stay on the owning CPU. */
#include "smp.h"

#define RUN_NS  200000000ull     /* 200 ms */

static volatile uint32_t ticks[MAX_CPUS];
static volatile uint32_t wrong_cpu;
static volatile uint32_t go, stop_all, finished;
static uint32_t period_ns[MAX_CPUS];

static void timer_handler(struct exc_frame *f, uint32_t *regs)
{
    int me = cpu_id();

    if (!(smp_read(SMP_TIMER_STATUS) & 1)) {
        wrong_cpu++;
    }
    smp_write(SMP_TIMER_STATUS, 1);
    ticks[me]++;
}

static void wake_handler(struct exc_frame *f, uint32_t *regs)
{
    smp_write(SMP_IPI_CLEAR, smp_read(SMP_IPI_PENDING));
}

static void run(int me)
{
    period_ns[me] = 1000000u * (me + 1);       /* cpu n: (n+1) ms */
    wait_event(go);
    smp_write(SMP_TIMER_INTERVAL, period_ns[me]);
    smp_write(SMP_TIMER_CTRL, TIMER_ENABLE | TIMER_PERIODIC);
    wait_event(stop_all);
    smp_write(SMP_TIMER_CTRL, 0);
    smp_write(SMP_TIMER_STATUS, 1);
}

void secondary_main(int cpu)
{
    set_ipl(0);
    run(cpu);
    atomic_inc(&finished);
    ipi_send(1u << 0, 1);           /* wake cpu0 waiting on "finished" */
}

int main(void)
{
    uint64_t c0, c1, r0, t0;

    set_handler(TIMER_VECTOR, timer_handler);
    set_handler(IPI_VECTOR, wake_handler);
    if (!bootinfo.smp_base) {
        printf("no virt-smp, skipping\n");
        return 0;
    }

    /* the global counter advances like the goldfish RTC */
    c0 = smp_counter_ns();
    r0 = rtc_ns(bootinfo.rtc_base);
    delay_ns(10000000);
    c1 = smp_counter_ns();
    printf("counter: %lu us over a 10 ms rtc delay\n",
           (unsigned long)((c1 - c0) / 1000));
    CHECK(c1 - c0 >= 9000000 && c1 - c0 < 100000000, "counter rate");
    (void)r0;

    /* one-shot on cpu0 */
    set_ipl(0);
    t0 = smp_counter_ns();
    smp_write(SMP_TIMER_INTERVAL, 5000000);
    smp_write(SMP_TIMER_CTRL, TIMER_ENABLE);
    wait_event(ticks[0] == 1);
    t0 = smp_counter_ns() - t0;
    CHECK(t0 >= 5000000, "one-shot fired after %lu us", (unsigned long)(t0 / 1000));
    CHECK((smp_read(SMP_TIMER_CTRL) & TIMER_ENABLE) == 0, "one-shot still enabled");
    delay_ns(20000000);
    CHECK(ticks[0] == 1, "one-shot fired %lu times", (unsigned long)ticks[0]);
    ticks[0] = 0;

    /* masked by IPL 6 */
    set_ipl(6);
    smp_write(SMP_TIMER_INTERVAL, 1000000);
    smp_write(SMP_TIMER_CTRL, TIMER_ENABLE);
    delay_ns(5000000);
    CHECK(ticks[0] == 0, "timer taken at IPL 6");
    CHECK(smp_read(SMP_TIMER_STATUS) == 1, "timer not pending");
    set_ipl(0);
    __asm__ volatile("nop" ::: "memory");
    CHECK(ticks[0] == 1, "pending timer not taken");
    ticks[0] = 0;

    if (smp_start_all() != ncpus) {
        panic("cpus did not start");
    }
    t0 = smp_counter_ns();
    go = 1;
    ipi_send(0xfffe, 1);           /* wake everybody */
    period_ns[0] = 1000000u;
    smp_write(SMP_TIMER_INTERVAL, period_ns[0]);
    smp_write(SMP_TIMER_CTRL, TIMER_ENABLE | TIMER_PERIODIC);
    delay_ns(RUN_NS);
    stop_all = 1;
    smp_write(SMP_TIMER_CTRL, 0);
    t0 = smp_counter_ns() - t0;
    ipi_send(0xfffe, 1);
    wait_event(finished == (uint32_t)ncpus - 1);

    for (int n = 0; n < ncpus; n++) {
        uint32_t want = t0 / period_ns[n];
        printf("  cpu%-2d period %2lu ms: %4lu ticks (expected ~%lu)\n", n,
               (unsigned long)period_ns[n] / 1000000,
               (unsigned long)ticks[n], (unsigned long)want);
        CHECK(ticks[n] + 2 >= want * 9 / 10 && ticks[n] <= want + 2,
              "cpu%d tick count", n);
    }
    CHECK(wrong_cpu == 0, "%lu timer interrupts without expiry",
          (unsigned long)wrong_cpu);
    return 0;
}
