/* s3: inter-processor interrupts.  Every CPU in turn pings every other
 * CPU, which idles in STOP and answers with an IPI back.  Also checks
 * self-IPIs, IPL masking of IPIs, and IPI_PENDING/IPI_CLEAR. */
#include "smp.h"

#define ROUNDS   50
#define R_PING   0      /* sender is in ping_from[target] */
#define R_ACK    1
#define R_SELF   2
#define R_WAKE   3

static volatile uint32_t recv[MAX_CPUS][MAX_CPUS];  /* [to][from] */
static volatile uint32_t acks[MAX_CPUS];
static volatile uint32_t self_ipis[MAX_CPUS];
static volatile uint32_t ipi_irqs[MAX_CPUS];
static volatile uint32_t turn, quit;
static volatile uint32_t ready;
static volatile uint32_t ping_from[MAX_CPUS];

static void ipi_handler(struct exc_frame *f, uint32_t *regs)
{
    int me = cpu_id();
    uint32_t pend = smp_read(SMP_IPI_PENDING);

    smp_write(SMP_IPI_CLEAR, pend);
    ipi_irqs[me]++;
    if (pend & (1u << R_PING)) {
        int from = ping_from[me];
        recv[me][from]++;
        ipi_send(1u << from, 1u << R_ACK);
    }
    if (pend & (1u << R_ACK)) {
        acks[me]++;
    }
    if (pend & (1u << R_SELF)) {
        self_ipis[me]++;
    }
}

static void master(int me)
{
    for (int to = 0; to < ncpus; to++) {
        if (to == me) {
            continue;
        }
        for (int r = 0; r < ROUNDS; r++) {
            uint32_t a = acks[me];
            uint64_t t = rtc_ns(bootinfo.rtc_base);

            ping_from[to] = me;
            ipi_send(1u << to, 1u << R_PING);
            wait_event(acks[me] != a);
            if (rtc_ns(bootinfo.rtc_base) - t > 1000000000ull) {
                panic("cpu%d: ack from cpu%d took more than 1s", me, to);
            }
        }
    }
}

static void loop(int me)
{
    set_ipl(0);
    atomic_inc(&ready);
    while (!quit) {
        wait_event(quit || turn == (uint32_t)me);
        if (turn == (uint32_t)me) {
            master(me);
            turn = me + 1;
            ipi_send(1u << 0, 1u << R_WAKE);    /* tell cpu0 */
        }
    }
}

void secondary_main(int cpu)
{
    loop(cpu);
    set_ipl(7);
    for (;;) {
        cpu_stop(7);
    }
}

int main(void)
{
    uint32_t before;

    set_handler(IPI_VECTOR, ipi_handler);
    if (ncpus < 2) {
        printf("needs at least 2 cpus, skipping\n");
        return 0;
    }
    CHECK((smp_read(SMP_IPI_CONFIG) & 0xff07) == ((IPI_VECTOR << 8) | 6),
          "IPI_CONFIG reset value %08lx", (unsigned long)smp_read(SMP_IPI_CONFIG));

    /* self IPI, masked then unmasked */
    set_ipl(6);
    ipi_send(1u << 0, 1u << R_SELF);
    CHECK(smp_read(SMP_IPI_PENDING) == (1u << R_SELF), "pending %08lx",
          (unsigned long)smp_read(SMP_IPI_PENDING));
    delay_ns(1000000);
    CHECK(self_ipis[0] == 0, "self IPI taken at IPL 6");
    set_ipl(5);
    __asm__ volatile("nop" ::: "memory");
    CHECK(self_ipis[0] == 1, "self IPI not taken at IPL 5");
    CHECK(smp_read(SMP_IPI_PENDING) == 0, "pending after handler");
    set_ipl(7);

    if (smp_start_all() != ncpus) {
        panic("cpus did not start");
    }
    while (ready != (uint32_t)ncpus - 1) {
    }

    /* ping-pong: turn 0 is cpu0, then each secondary in turn */
    set_ipl(0);
    master(0);
    turn = 1;
    for (int n = 1; n < ncpus; n++) {
        ipi_send(1u << n, 1u << R_WAKE);    /* spare reason: just a wakeup */
        wait_event(turn != (uint32_t)n);
    }

    /* an IPI stays pending while masked at IPL 6 */
    before = ipi_irqs[0];
    set_ipl(6);
    ipi_send(1u << 0, 1u << R_SELF);
    delay_ns(2000000);
    CHECK(ipi_irqs[0] == before, "IPI taken while masked");
    set_ipl(0);
    __asm__ volatile("nop" ::: "memory");
    CHECK(ipi_irqs[0] == before + 1, "masked IPI lost");

    quit = 1;
    ipi_send(0xffff & ~1u, 1u << R_WAKE);
    set_ipl(7);

    for (int to = 0; to < ncpus; to++) {
        for (int from = 0; from < ncpus; from++) {
            uint32_t want = (to == from) ? 0 : ROUNDS;
            CHECK(recv[to][from] == want, "cpu%d got %lu IPIs from cpu%d",
                  to, (unsigned long)recv[to][from], from);
        }
    }
    printf("%d cpus: %d pings per ordered pair, all acknowledged\n",
           ncpus, ROUNDS);
    return 0;
}
