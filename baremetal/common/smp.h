/* virt-smp controller access (see doc/virt-smp-spec.md). */
#ifndef SMP_H
#define SMP_H

#include "lib.h"

#define SMP_ID              0x000
#define SMP_NCPUS           0x004
#define SMP_VERSION         0x008
#define SMP_IPI_PENDING     0x010
#define SMP_IPI_CLEAR       0x014
#define SMP_IPI_CONFIG      0x018
#define SMP_TIMER_CTRL      0x020
#define SMP_TIMER_INTERVAL  0x024
#define SMP_TIMER_STATUS    0x028
#define SMP_TIMER_CONFIG    0x02c
#define SMP_IPI_SEND        0x100
#define SMP_DEVIRQ_ROUTE    0x110
#define SMP_COUNTER_LO      0x120
#define SMP_COUNTER_HI      0x124
#define SMP_CPU(n)          (0x200 + 16 * (n))
#define SMP_START_PC        0x0
#define SMP_START_SP        0x4
#define SMP_START_ARG       0x8
#define SMP_CPU_CTRL        0xc

#define TIMER_ENABLE        1
#define TIMER_PERIODIC      2

#define IPI_VECTOR          0x40
#define TIMER_VECTOR        0x41

extern int ncpus;                   /* 1 if no virt-smp */
extern volatile uint32_t cpu_online[MAX_CPUS];

static inline uint32_t smp_read(uint32_t reg)
{
    return mmio_read32(bootinfo.smp_base + reg);
}
static inline void smp_write(uint32_t reg, uint32_t v)
{
    mmio_write32(bootinfo.smp_base + reg, v);
}
static inline int cpu_id(void)
{
    return bootinfo.smp_base ? (int)smp_read(SMP_ID) : 0;
}
static inline void ipi_send(uint32_t mask, uint32_t reasons)
{
    smp_write(SMP_IPI_SEND, (reasons << 16) | (mask & 0xffff));
}
uint64_t smp_counter_ns(void);

/* Start CPU n at _secondary_start; returns 0 when it is online. */
int smp_start_cpu(int n);
/* Start all secondary CPUs; returns how many came online. */
int smp_start_all(void);
/* Barrier across all ncpus CPUs (they must all be started) */
void smp_barrier(void);

#endif
