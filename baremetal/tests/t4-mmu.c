/* t4: 68040 MMU with 4 KiB pages and 3-level tables.
 * Identity-maps the first 16 MiB and the MMIO window, plus a test page
 * at VA 0x40000000.  Checks translation, U/M bit updates by the table
 * walk, PFLUSH after a remap, write protection and invalid pages. */
#include "lib.h"
#include "mmu040.h"

static uint32_t root[128] __attribute__((aligned(512)));
static uint32_t ptr_low[128] __attribute__((aligned(512)));
static uint32_t ptr_test[128] __attribute__((aligned(512)));
static uint32_t ptr_io[128] __attribute__((aligned(512)));
static uint32_t pt_ram[64][64] __attribute__((aligned(256)));
static uint32_t pt_test[64] __attribute__((aligned(256)));
static uint32_t pt_io[64] __attribute__((aligned(256)));
static uint32_t page_a[1024] __attribute__((aligned(4096)));
static uint32_t page_b[1024] __attribute__((aligned(4096)));

#define TEST_VA 0x40000000u
static volatile uint32_t recover_pc;   /* 0: unexpected fault */
static volatile uint32_t fault_addr;

static void access_fault(struct exc_frame *f, uint32_t *regs)
{
    if (!recover_pc) {
        panic("unexpected access fault at pc %p", (void *)f->pc);
    }
    if (EXC_FORMAT(f) == 7) {
        uint16_t *ssw = (uint16_t *)((char *)f + 0x0c);
        fault_addr = *(uint32_t *)((char *)f + 0x14);
        *ssw &= 0x0fff;
    }
    f->pc = recover_pc;
    recover_pc = 0;
}

/* read/write that recover from an access fault, return 0 if faulted */
static int try_read(volatile uint32_t *p, uint32_t *val)
{
    uint32_t before = exc_count[2], v = 0;
    __asm__ volatile("lea 1f,%%a0; movel %%a0,%1; movel %2@,%0; 1:"
                     : "+d"(v), "=m"(recover_pc) : "a"(p) : "a0", "memory");
    *val = v;
    return exc_count[2] == before;
}

static int try_write(volatile uint32_t *p, uint32_t val)
{
    uint32_t before = exc_count[2];
    __asm__ volatile("lea 1f,%%a0; movel %%a0,%0; movel %2,%1@; 1:"
                     : "=m"(recover_pc) : "a"(p), "d"(val) : "a0", "memory");
    return exc_count[2] == before;
}

int main(void)
{
    uint32_t v;
    int i, j;

    set_handler(2, access_fault);
    for (i = 0; i < 1024; i++) {
        page_a[i] = 0xaaaa0000 | i;
        page_b[i] = 0xbbbb0000 | i;
    }

    /* 0x00000000-0x00ffffff identity, copyback */
    root[0] = (uint32_t)ptr_low | UDT_RESIDENT;
    for (i = 0; i < 64; i++) {
        ptr_low[i] = (uint32_t)pt_ram[i] | UDT_RESIDENT;
        for (j = 0; j < 64; j++) {
            pt_ram[i][j] = ((i * 64 + j) << 12) | PDT_RESIDENT | PG_CM_CB;
        }
    }
    /* test page */
    root[RI(TEST_VA)] = (uint32_t)ptr_test | UDT_RESIDENT;
    ptr_test[PI(TEST_VA)] = (uint32_t)pt_test | UDT_RESIDENT;
    pt_test[0] = (uint32_t)page_a | PDT_RESIDENT | PG_CM_CB;
    /* MMIO 0xff000000-0xff03ffff identity, serialized */
    root[RI(0xff000000)] = (uint32_t)ptr_io | UDT_RESIDENT;
    ptr_io[PI(0xff000000)] = (uint32_t)pt_io | UDT_RESIDENT;
    for (j = 0; j < 64; j++) {
        pt_io[j] = (0xff000000u + (j << 12)) | PDT_RESIDENT | PG_CM_NC_SER;
    }

    mmu040_enable((uint32_t)root);
    printf("MMU enabled, tc=%04lx\n", (unsigned long)mmu040_get_tc());
    CHECK(mmu040_get_tc() & TC_E, "tc");

    volatile uint32_t *va = (volatile uint32_t *)TEST_VA;
    CHECK(!(pt_test[0] & (PG_U | PG_M)), "U/M set before access");
    CHECK(va[5] == (0xaaaa0000 | 5), "read through mapping: %08lx",
          (unsigned long)va[5]);
    CHECK(pt_test[0] & PG_U, "U not set after read");
    CHECK(!(pt_test[0] & PG_M), "M set after read");
    CHECK(ptr_test[0] & UDT_U, "U not set in pointer descriptor");
    CHECK(root[RI(TEST_VA)] & UDT_U, "U not set in root descriptor");
    va[6] = 0x12345678;
    CHECK(page_a[6] == 0x12345678, "write through mapping");
    CHECK(pt_test[0] & PG_M, "M not set after write");

    /* Remap to page B, flush that entry. */
    pt_test[0] = (uint32_t)page_b | PDT_RESIDENT | PG_CM_CB;
    pflush_page(TEST_VA);
    CHECK(va[5] == (0xbbbb0000 | 5), "read after remap: %08lx",
          (unsigned long)va[5]);

    /* Write protection */
    pt_test[0] |= PG_W;
    pflush_page(TEST_VA);
    CHECK(try_read(va, &v) && v == 0xbbbb0000, "read of RO page");
    CHECK(!try_write(va, 1), "write to RO page did not fault");
    CHECK(fault_addr == TEST_VA, "fault address %08lx",
          (unsigned long)fault_addr);
    CHECK(page_b[0] == 0xbbbb0000, "RO page modified");

    /* Invalid page */
    CHECK(!try_read(va + 1024, &v), "read of invalid page did not fault");
    CHECK(fault_addr == TEST_VA + 4096, "fault address %08lx",
          (unsigned long)fault_addr);

    /* PFLUSHA after invalidating the whole test pointer table */
    root[RI(TEST_VA)] = 0;
    pflusha();
    CHECK(!try_read(va, &v), "read after root invalidation did not fault");

    mmu040_disable();
    printf("MMU disabled, %lu access faults\n", (unsigned long)exc_count[2]);
    return 0;
}
