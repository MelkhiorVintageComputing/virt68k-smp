/* s5: memory-ordering litmus tests between CPU0 and CPU1.
 * QEMU implements m68k with TCG_MO_ALL (sequential consistency for plain
 * accesses), so both classic forbidden outcomes must never be observed:
 *   MP: W data; W flag  ||  R flag; R data     forbidden: flag=1, data=0
 *   SB: W x; R y        ||  W y; R x           forbidden: 0, 0           */
#include "smp.h"

#define TRIALS 20000
#define MAX_NS 3000000000ull  /* single-threaded TCG is slow at this */

static volatile uint32_t go, done1;
static volatile uint32_t data, flag, x, y;
static volatile uint32_t r_flag, r_data, r1, r2;
static volatile uint32_t which;     /* 0: MP, 1: SB */

void secondary_main(int cpu)
{
    uint32_t t = 0;

    if (cpu != 1) {
        return;
    }
    for (;;) {
        while (go == t) {
        }
        t = go;
        if (which == 0) {
            r_flag = flag;
            r_data = data;
        } else {
            y = 1;
            r2 = x;
        }
        done1 = t;
    }
}

static uint32_t run(int test)
{
    uint32_t bad = 0, seen_reorder_free = 0, t;
    uint64_t t_end = rtc_ns(bootinfo.rtc_base) + MAX_NS;

    which = test;
    for (t = 1; t <= TRIALS && rtc_ns(bootinfo.rtc_base) < t_end; t++) {
        data = flag = x = y = 0;
        r_flag = r_data = r1 = r2 = ~0u;
        go = go + 1;
        if (test == 0) {
            data = 1;
            flag = 1;
        } else {
            x = 1;
            r1 = y;
        }
        while (done1 != go) {
        }
        if (test == 0) {
            bad += r_flag == 1 && r_data == 0;
            seen_reorder_free += r_flag == 0;
        } else {
            bad += r1 == 0 && r2 == 0;
            seen_reorder_free += r1 == 0 || r2 == 0;
        }
    }
    printf("%s: %lu trials, %lu forbidden, %lu interleaved\n",
           test == 0 ? "MP" : "SB", (unsigned long)t - 1, (unsigned long)bad,
           (unsigned long)seen_reorder_free);
    return bad;
}

int main(void)
{
    if (ncpus < 2) {
        printf("needs at least 2 cpus, skipping\n");
        return 0;
    }
    if (smp_start_all() != ncpus) {
        panic("cpus did not start");
    }
    CHECK(run(0) == 0, "MP forbidden outcome observed");
    CHECK(run(1) == 0, "SB forbidden outcome observed");
    return 0;
}
