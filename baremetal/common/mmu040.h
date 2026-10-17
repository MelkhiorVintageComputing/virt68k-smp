/* 68040 MMU helpers (4 KiB pages). */
#ifndef MMU040_H
#define MMU040_H

#include <stdint.h>

/* root / pointer table descriptors */
#define UDT_RESIDENT  0x00000002u
#define UDT_W         0x00000004u
#define UDT_U         0x00000008u
/* page descriptors */
#define PDT_RESIDENT  0x00000001u
#define PG_W          0x00000004u
#define PG_U          0x00000008u
#define PG_M          0x00000010u
#define PG_CM_WT      0x00000000u
#define PG_CM_CB      0x00000020u
#define PG_CM_NC_SER  0x00000040u
#define PG_CM_NC      0x00000060u
#define PG_S          0x00000080u
#define PG_G          0x00000400u

#define TC_E          0x8000u

/* table indices for 4 KiB pages */
#define RI(va)  (((uint32_t)(va) >> 25) & 0x7f)
#define PI(va)  (((uint32_t)(va) >> 18) & 0x7f)
#define PGI(va) (((uint32_t)(va) >> 12) & 0x3f)

static inline void pflusha(void)
{
    __asm__ volatile("pflusha" ::: "memory");
}

static inline void pflush_page(uint32_t va)
{
    /* PFLUSH (An) uses DFC as the function code: flush supervisor data
     * and program (5, 6) and user (1, 2) entries. */
    uint32_t fc;
    for (fc = 1; fc <= 6; fc++) {
        __asm__ volatile("movec %0,%%dfc; pflush %1@"
                         : : "d"(fc), "a"(va) : "memory");
    }
}

static inline uint32_t mmu040_get_tc(void)
{
    uint32_t tc;
    __asm__ volatile("movec %%tc,%0" : "=d"(tc));
    return tc;
}

static inline void mmu040_enable(uint32_t root)
{
    __asm__ volatile("movec %0,%%srp; movec %0,%%urp; pflusha; movec %1,%%tc"
                     : : "d"(root), "d"(TC_E) : "memory");
}

static inline void mmu040_disable(void)
{
    __asm__ volatile("movec %0,%%tc; pflusha" : : "d"(0) : "memory");
}

#endif
