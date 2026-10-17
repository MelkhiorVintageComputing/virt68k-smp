/* t5: single-CPU semantics of TAS, CAS (b/w/l) and CAS2 (w/l). */
#include "lib.h"

static volatile uint8_t  b;
static volatile uint16_t w;
static volatile uint32_t l, l2;
static volatile uint16_t w2;
/* adjacent and non-adjacent CAS2.L operands */
static volatile uint32_t pair[2];

static uint16_t ccr(void)
{
    uint16_t c;
    __asm__ volatile("movew %%ccr,%0" : "=d"(c));
    return c & 0x1f;
}

#define CCR_Z 0x04
#define CCR_N 0x08

int main(void)
{
    uint32_t c;
    uint16_t cc;

    /* TAS: sets bit 7, flags from the old value */
    b = 0x05;
    CHECK(!tas(&b), "tas on clear byte reported set");
    CHECK(b == 0x85, "tas result %02x", b);
    CHECK(tas(&b), "tas on set byte reported clear");
    b = 0;
    __asm__ volatile("tas %0" : "+m"(b) : : "cc");
    cc = ccr();
    CHECK(cc & CCR_Z, "tas Z flag on zero byte");

    /* CAS.L success / failure */
    l = 100;
    c = cas32(&l, 100, 200);
    CHECK(c == 100 && l == 200, "casl success: c=%lu l=%lu",
          (unsigned long)c, (unsigned long)l);
    c = cas32(&l, 100, 300);
    CHECK(c == 200 && l == 200, "casl failure: c=%lu l=%lu",
          (unsigned long)c, (unsigned long)l);

    /* CAS.W, CAS.B */
    w = 0x1234;
    {
        uint16_t cw = 0x1234;
        __asm__ volatile("casw %0,%2,%1" : "+d"(cw), "+m"(w) : "d"(0x5678) : "cc");
        CHECK(w == 0x5678, "casw success %04x", w);
        cw = 0x1234;
        __asm__ volatile("casw %0,%2,%1" : "+d"(cw), "+m"(w) : "d"(0x9999) : "cc");
        CHECK(w == 0x5678 && cw == 0x5678, "casw failure %04x %04x", w, cw);
    }
    b = 7;
    {
        uint8_t cb = 7;
        __asm__ volatile("casb %0,%2,%1" : "+d"(cb), "+m"(b) : "d"(9) : "cc");
        CHECK(b == 9, "casb success %u", b);
    }

    /* atomic_inc loop */
    l = 0;
    for (int i = 0; i < 1000; i++) {
        atomic_inc(&l);
    }
    CHECK(l == 1000, "atomic_inc %lu", (unsigned long)l);

    /* CAS2.L on two separate locations */
    l = 1, l2 = 2;
    {
        register uint32_t c1 __asm__("d0") = 1;
        register uint32_t c2 __asm__("d1") = 2;
        register uint32_t u1 __asm__("d2") = 10;
        register uint32_t u2 __asm__("d3") = 20;
        register volatile uint32_t *a1 __asm__("a0") = &l;
        register volatile uint32_t *a2 __asm__("a1") = &l2;
        __asm__ volatile("cas2l %%d0:%%d1,%%d2:%%d3,(%%a0):(%%a1)"
                         : "+d"(c1), "+d"(c2)
                         : "d"(u1), "d"(u2), "a"(a1), "a"(a2)
                         : "cc", "memory");
        CHECK(l == 10 && l2 == 20, "cas2l success: %lu %lu",
              (unsigned long)l, (unsigned long)l2);
        c1 = 10, c2 = 99;   /* second compare fails: nothing written */
        __asm__ volatile("cas2l %%d0:%%d1,%%d2:%%d3,(%%a0):(%%a1)"
                         : "+d"(c1), "+d"(c2)
                         : "d"(u1), "d"(u2), "a"(a1), "a"(a2)
                         : "cc", "memory");
        CHECK(l == 10 && l2 == 20, "cas2l failure wrote: %lu %lu",
              (unsigned long)l, (unsigned long)l2);
        CHECK(c1 == 10 && c2 == 20, "cas2l failure loaded: %lu %lu",
              (unsigned long)c1, (unsigned long)c2);
    }

    /* CAS2.L on adjacent words (the 64-bit fast path in QEMU) */
    pair[0] = 5, pair[1] = 6;
    {
        register uint32_t c1 __asm__("d0") = 5;
        register uint32_t c2 __asm__("d1") = 6;
        register uint32_t u1 __asm__("d2") = 50;
        register uint32_t u2 __asm__("d3") = 60;
        register volatile uint32_t *a1 __asm__("a0") = &pair[0];
        register volatile uint32_t *a2 __asm__("a1") = &pair[1];
        __asm__ volatile("cas2l %%d0:%%d1,%%d2:%%d3,(%%a0):(%%a1)"
                         : "+d"(c1), "+d"(c2)
                         : "d"(u1), "d"(u2), "a"(a1), "a"(a2)
                         : "cc", "memory");
        CHECK(pair[0] == 50 && pair[1] == 60, "cas2l adjacent: %lu %lu",
              (unsigned long)pair[0], (unsigned long)pair[1]);
    }

    /* CAS2.W */
    w = 1, w2 = 2;
    {
        register uint32_t c1 __asm__("d0") = 1;
        register uint32_t c2 __asm__("d1") = 2;
        register uint32_t u1 __asm__("d2") = 3;
        register uint32_t u2 __asm__("d3") = 4;
        register volatile uint16_t *a1 __asm__("a0") = &w;
        register volatile uint16_t *a2 __asm__("a1") = &w2;
        __asm__ volatile("cas2w %%d0:%%d1,%%d2:%%d3,(%%a0):(%%a1)"
                         : "+d"(c1), "+d"(c2)
                         : "d"(u1), "d"(u2), "a"(a1), "a"(a2)
                         : "cc", "memory");
        CHECK(w == 3 && w2 == 4, "cas2w: %u %u", w, w2);
    }
    return 0;
}
