/* s6: 68040 MMU on all CPUs with shared page tables.
 *  1. every CPU sees the same mapping;
 *  2. remap + IPI-driven PFLUSH shootdown: every CPU sees the new page;
 *  3. U/M bits are set by every CPU's table walks (each writes its own page);
 *  4. race stress: CPU0 keeps rewriting a PTE while the other CPUs force
 *     table walks through it; the walk must never write back a stale
 *     descriptor (it may only add U/M bits to what CPU0 wrote). */
#include "smp.h"
#include "mmu040.h"

static uint32_t root[128] __attribute__((aligned(512)));
static uint32_t ptr_low[128] __attribute__((aligned(512)));
static uint32_t ptr_test[128] __attribute__((aligned(512)));
static uint32_t ptr_io[128] __attribute__((aligned(512)));
static uint32_t pt_ram[64][64] __attribute__((aligned(256)));
static volatile uint32_t pt_test[64] __attribute__((aligned(256)));
static uint32_t pt_io[64] __attribute__((aligned(256)));
static uint32_t pages[2 + MAX_CPUS][1024] __attribute__((aligned(4096)));

#define TEST_VA     0x40000000u
#define VA(slot)    (TEST_VA + (slot) * 4096u)
#define SLOT_SHARED 0
#define SLOT_RACE   1
#define SLOT_CPU(n) (2 + (n))
#define PAGE_A      0
#define PAGE_B      1
#define R_SHOOT     0
#define R_WAKE      1
#define RACE_NS     300000000ull

static volatile uint32_t phase, phase_done;
static volatile uint32_t seen[MAX_CPUS];
static volatile uint32_t shot[MAX_CPUS];
static volatile uint32_t race_stop;
static volatile uint32_t walks[MAX_CPUS];

static uint32_t pte(int page)
{
    return (uint32_t)pages[page] | PDT_RESIDENT | PG_CM_CB;
}

static void ipi_handler(struct exc_frame *f, uint32_t *regs)
{
    uint32_t pend = smp_read(SMP_IPI_PENDING);
    int me = cpu_id();

    smp_write(SMP_IPI_CLEAR, pend);
    if (pend & (1u << R_SHOOT)) {
        pflush_page(VA(SLOT_SHARED));
        shot[me]++;
        ipi_send(1u << 0, 1u << R_WAKE);    /* acknowledge */
    }
}

static void done_phase(void)
{
    atomic_inc(&phase_done);
    ipi_send(1u << 0, 1u << R_WAKE);
}

static void secondary_work(int me)
{
    volatile uint32_t *shared = (volatile uint32_t *)VA(SLOT_SHARED);
    volatile uint32_t *mine = (volatile uint32_t *)VA(SLOT_CPU(me));
    volatile uint32_t *race = (volatile uint32_t *)VA(SLOT_RACE);

    mmu040_enable((uint32_t)root);

    wait_event(phase == 1);
    seen[me] = shared[3];
    done_phase();

    wait_event(phase == 2);         /* after the shootdown */
    seen[me] = shared[3];
    mine[0] = me;                   /* sets M on our own page */
    done_phase();

    wait_event(phase == 3);
    while (!race_stop) {
        pflush_page(VA(SLOT_RACE));
        (void)race[0];              /* force a table walk */
        walks[me]++;
    }
    done_phase();
}

void secondary_main(int cpu)
{
    set_ipl(0);
    secondary_work(cpu);
    for (;;) {
        cpu_stop(0);
    }
}

static void next_phase(int p)
{
    phase_done = 0;
    phase = p;
    ipi_send(0xfffe, 1u << R_WAKE);
    wait_event(phase_done == (uint32_t)ncpus - 1);
}

int main(void)
{
    volatile uint32_t *shared = (volatile uint32_t *)VA(SLOT_SHARED);
    uint32_t stale = 0, rewrites = 0;
    uint64_t t_end;
    int i, j;

    set_handler(IPI_VECTOR, ipi_handler);
    if (ncpus < 2) {
        printf("needs at least 2 cpus, skipping\n");
        return 0;
    }
    for (i = 0; i < 1024; i++) {
        pages[PAGE_A][i] = 0xaaaa0000 | i;
        pages[PAGE_B][i] = 0xbbbb0000 | i;
    }
    root[0] = (uint32_t)ptr_low | UDT_RESIDENT;
    for (i = 0; i < 64; i++) {
        ptr_low[i] = (uint32_t)pt_ram[i] | UDT_RESIDENT;
        for (j = 0; j < 64; j++) {
            pt_ram[i][j] = ((i * 64 + j) << 12) | PDT_RESIDENT | PG_CM_CB;
        }
    }
    root[RI(TEST_VA)] = (uint32_t)ptr_test | UDT_RESIDENT;
    ptr_test[PI(TEST_VA)] = (uint32_t)pt_test | UDT_RESIDENT;
    pt_test[SLOT_SHARED] = pte(PAGE_A);
    pt_test[SLOT_RACE] = pte(PAGE_A);
    for (i = 0; i < ncpus; i++) {
        pt_test[SLOT_CPU(i)] = pte(2 + i);
    }
    root[RI(0xff000000)] = (uint32_t)ptr_io | UDT_RESIDENT;
    ptr_io[PI(0xff000000)] = (uint32_t)pt_io | UDT_RESIDENT;
    for (j = 0; j < 64; j++) {
        pt_io[j] = (0xff000000u + (j << 12)) | PDT_RESIDENT | PG_CM_NC_SER;
    }

    mmu040_enable((uint32_t)root);
    if (smp_start_all() != ncpus) {
        panic("cpus did not start");
    }
    set_ipl(0);

    /* 1: everybody sees page A */
    next_phase(1);
    for (i = 1; i < ncpus; i++) {
        CHECK(seen[i] == (0xaaaa0000 | 3), "cpu%d sees %08lx", i,
              (unsigned long)seen[i]);
    }

    /* 2: remap to page B, shoot down the old translation everywhere */
    pt_test[SLOT_SHARED] = pte(PAGE_B);
    pflush_page(VA(SLOT_SHARED));
    ipi_send(0xfffe, 1u << R_SHOOT);
    for (i = 1; i < ncpus; i++) {
        wait_event(shot[i] == 1);
    }
    CHECK(shared[3] == (0xbbbb0000 | 3), "cpu0 after remap");
    next_phase(2);
    for (i = 1; i < ncpus; i++) {
        CHECK(seen[i] == (0xbbbb0000 | 3), "cpu%d after shootdown: %08lx", i,
              (unsigned long)seen[i]);
        CHECK(pages[2 + i][0] == (uint32_t)i, "cpu%d write", i);
        CHECK((pt_test[SLOT_CPU(i)] & (PG_U | PG_M)) == (PG_U | PG_M),
              "cpu%d U/M bits %08lx", i, (unsigned long)pt_test[SLOT_CPU(i)]);
    }

    /* 3: race between PTE rewrites and table walks on other CPUs */
    phase_done = 0;
    phase = 3;
    ipi_send(0xfffe, 1u << R_WAKE);
    t_end = rtc_ns(bootinfo.rtc_base) + RACE_NS;
    for (i = 0; rtc_ns(bootinfo.rtc_base) < t_end; i++) {
        uint32_t want = pte(i & 1 ? PAGE_B : PAGE_A);
        rewrites++;
        pt_test[SLOT_RACE] = want;
        for (j = 0; j < 20; j++) {
            __asm__ volatile("nop");
        }
        if ((pt_test[SLOT_RACE] & ~(PG_U | PG_M)) != want) {
            stale++;
        }
    }
    race_stop = 1;
    wait_event(phase_done == (uint32_t)ncpus - 1);
    {
        uint32_t w = 0;
        for (i = 1; i < ncpus; i++) {
            w += walks[i];
        }
        printf("race: %lu PTE rewrites, %lu concurrent walks, %lu stale write-backs\n",
               (unsigned long)rewrites, (unsigned long)w, (unsigned long)stale);
    }
    CHECK(stale == 0, "table walk overwrote a PTE update");

    set_ipl(7);
    mmu040_disable();
    return 0;
}
