/* t2: VBR relocation, traps, illegal instruction, divide by zero,
 * access fault on unmapped memory, and SR/IPL manipulation. */
#include "lib.h"

static volatile uint32_t trap_d0;
static volatile uint32_t recover_pc;

static void trap0(struct exc_frame *f, uint32_t *regs)
{
    trap_d0 = regs[0];
    regs[0] = 0x600df00d;
}

static void illegal(struct exc_frame *f, uint32_t *regs)
{
    f->pc += 2;         /* skip the ILLEGAL opcode */
}

static void divzero(struct exc_frame *f, uint32_t *regs)
{
    /* PC already points past the divide */
}

static void access_fault(struct exc_frame *f, uint32_t *regs)
{
    printf("  access fault: format %d pc=%p\n", EXC_FORMAT(f), (void *)f->pc);
    if (EXC_FORMAT(f) == 7) {
        /* 68040 access error frame: clear the SSW continuation bits
         * (CP/CU/CT/CM) so RTE does not restart the access, and resume
         * at the recovery point. */
        uint16_t *ssw = (uint16_t *)((char *)f + 0x0c);
        *ssw &= 0x0fff;
    }
    f->pc = recover_pc;
}

int main(void)
{
    uint32_t vbr, r, q;

    __asm__ volatile("movec %%vbr,%0" : "=d"(vbr));
    printf("vbr=%p (_vectors=%p)\n", (void *)vbr, _vectors);
    CHECK(vbr == (uint32_t)_vectors, "vbr");

    /* trap #0 with a value passed in d0 and replaced by the handler */
    set_handler(32, trap0);
    {
        register uint32_t d0 __asm__("d0") = 0x12345678;
        __asm__ volatile("trap #0" : "+d"(d0) : : "memory");
        r = d0;
    }
    CHECK(trap_d0 == 0x12345678, "trap arg %lx", (unsigned long)trap_d0);
    CHECK(r == 0x600df00d, "trap ret %lx", (unsigned long)r);
    CHECK(exc_count[32] == 1, "trap count");

    set_handler(4, illegal);
    __asm__ volatile(".word 0x4afc" ::: "memory");
    CHECK(exc_count[4] == 1, "illegal count");

    set_handler(5, divzero);
    {
        uint32_t z = 0;
        __asm__ volatile("" : "+d"(z));
        q = 100;
        __asm__ volatile("divul %1,%0" : "+d"(q) : "d"(z) : "cc");
    }
    CHECK(exc_count[5] == 1, "divzero count");

    /* SR / IPL */
    set_ipl(3);
    CHECK(((get_sr() >> 8) & 7) == 3, "ipl 3");
    set_ipl(0);
    CHECK(((get_sr() >> 8) & 7) == 0, "ipl 0");
    CHECK(get_sr() & 0x2000, "supervisor");
    set_ipl(7);

    /* Access to unmapped physical memory: is it an access fault? */
    set_handler(2, access_fault);
    {
        volatile uint32_t *bad = (volatile uint32_t *)0xf0000000;
        __asm__ volatile("lea 1f,%%a0; movel %%a0,%0; movel %1@,%%d0; 1:"
                         : "=m"(recover_pc) : "a"(bad) : "a0", "d0", "memory");
        CHECK(exc_count[2] == 1, "access fault count");
        printf("  unmapped read: %lu access faults\n",
               (unsigned long)exc_count[2]);
    }
    return 0;
}
