/* t3: periodic interrupts from the second goldfish-rtc (the "timer",
 * as used by NetBSD/virt68k), delivered at IPL 6 through goldfish-pic
 * #6, with STOP-based idling and IPL masking. */
#include "lib.h"

#define PERIOD_NS 1000000ull    /* 1 ms */
#define TICKS     200

static uintptr_t timer;
static uint32_t timer_irq;
static volatile uint32_t ticks;
static uint64_t next_alarm;

static void timer_intr(struct exc_frame *f, uint32_t *regs)
{
    mmio_write32(timer + RTC_CLEAR_INTERRUPT, 1);
    ticks++;
    next_alarm += PERIOD_NS;
    rtc_set_alarm(timer, next_alarm);
}

int main(void)
{
    uint64_t t0, t1;
    uint32_t stops = 0, before;

    timer = bootinfo.rtc_base + 0x1000;
    timer_irq = bootinfo.rtc_irq + 1;
    printf("timer at %p irq %lu (pic %lu line %lu)\n", (void *)timer,
           (unsigned long)timer_irq, (unsigned long)PIC_OF_IRQ(timer_irq) + 1,
           (unsigned long)LINE_OF_IRQ(timer_irq));
    set_handler(24 + 6, timer_intr);
    pic_enable(timer_irq);
    mmio_write32(timer + RTC_IRQ_ENABLED, 1);

    t0 = rtc_ns(bootinfo.rtc_base);
    next_alarm = rtc_ns(timer) + PERIOD_NS;
    rtc_set_alarm(timer, next_alarm);
    while (ticks < TICKS) {
        cpu_stop(0);        /* sleep until the next interrupt */
        stops++;
    }
    t1 = rtc_ns(bootinfo.rtc_base);
    printf("%lu ticks in %lu us, %lu wakeups\n", (unsigned long)ticks,
           (unsigned long)((t1 - t0) / 1000), (unsigned long)stops);
    CHECK(t1 - t0 >= TICKS * PERIOD_NS * 9 / 10, "too fast");
    CHECK(t1 - t0 <= TICKS * PERIOD_NS * 10, "too slow");
    CHECK(exc_count[30] == ticks, "vector 30 count");

    /* Masked at IPL 6: no interrupt may be taken. */
    set_ipl(6);
    before = ticks;
    delay_ns(10 * PERIOD_NS);
    CHECK(ticks == before, "interrupt taken at IPL 6 (%lu)",
          (unsigned long)(ticks - before));
    /* Unmasking delivers the pending one. */
    set_ipl(5);
    __asm__ volatile("nop" ::: "memory");
    /* (the handler then catches up on the missed periods) */
    CHECK(ticks > before, "pending interrupt not delivered (%lu)",
          (unsigned long)(ticks - before));

    set_ipl(7);
    mmio_write32(timer + RTC_IRQ_ENABLED, 0);
    mmio_write32(timer + RTC_CLEAR_ALARM, 1);
    return 0;
}
