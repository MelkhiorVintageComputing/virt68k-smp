/* Minimal support library for bare-metal tests on QEMU m68k "virt". */
#ifndef LIB_H
#define LIB_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define MAX_CPUS 16

/* ---- MMIO ---- */
static inline uint32_t mmio_read32(uintptr_t a)
{
    return *(volatile uint32_t *)a;
}
static inline void mmio_write32(uintptr_t a, uint32_t v)
{
    *(volatile uint32_t *)a = v;
}

/* ---- bootinfo ---- */
#define BI_LAST              0x0000
#define BI_MACHTYPE          0x0001
#define BI_CPUTYPE           0x0002
#define BI_FPUTYPE           0x0003
#define BI_MMUTYPE           0x0004
#define BI_MEMCHUNK          0x0005
#define BI_RAMDISK           0x0006
#define BI_COMMAND_LINE      0x0007
#define BI_RNG_SEED          0x0008
#define BI_VIRT_QEMU_VERSION 0x8000
#define BI_VIRT_GF_PIC_BASE  0x8001
#define BI_VIRT_GF_RTC_BASE  0x8002
#define BI_VIRT_GF_TTY_BASE  0x8003
#define BI_VIRT_VIRTIO_BASE  0x8004
#define BI_VIRT_CTRL_BASE    0x8005
#define BI_VIRT_SMP_BASE     0x8007   /* new, see doc/virt-smp-spec.md */

struct bi_record {
    uint16_t tag;
    uint16_t size;          /* including this header */
    uint32_t data[];
};

struct bootinfo {
    uint32_t machtype, cputype, fputype, mmutype;
    uint32_t mem_base, mem_size;
    uint32_t qemu_version;
    uint32_t pic_base, pic_irq;
    uint32_t rtc_base, rtc_irq;
    uint32_t tty_base, tty_irq;
    uint32_t virtio_base, virtio_irq;
    uint32_t ctrl_base, ctrl_irq;
    uint32_t smp_base, smp_irq;     /* 0 if absent */
    const char *cmdline;
    int nrecords;
};

extern struct bootinfo bootinfo;
const struct bi_record *bootinfo_first(void);
const struct bi_record *bootinfo_next(const struct bi_record *r);
void bootinfo_parse(void);

/* Goldfish PIC: bootinfo irq number -> (pic index 0..5, line 0..31) */
#define PIC_OF_IRQ(irq)   (((irq) - 8) / 32)
#define LINE_OF_IRQ(irq)  (((irq) - 8) % 32)
#define PIC_REG_STATUS       0x00
#define PIC_REG_IRQ_PENDING  0x04
#define PIC_REG_DISABLE_ALL  0x08
#define PIC_REG_DISABLE      0x0c
#define PIC_REG_ENABLE       0x10
void pic_enable(uint32_t irq);
void pic_disable(uint32_t irq);

/* Goldfish RTC (ns time base, absolute alarm) */
#define RTC_TIME_LOW         0x00
#define RTC_TIME_HIGH        0x04
#define RTC_ALARM_LOW        0x08
#define RTC_ALARM_HIGH       0x0c
#define RTC_IRQ_ENABLED      0x10
#define RTC_CLEAR_ALARM      0x14
#define RTC_ALARM_STATUS     0x18
#define RTC_CLEAR_INTERRUPT  0x1c
uint64_t rtc_ns(uintptr_t rtc);
void rtc_set_alarm(uintptr_t rtc, uint64_t abs_ns);
void delay_ns(uint64_t ns);

/* ---- console / control ---- */
void putchar_raw(int c);
void puts_raw(const char *s);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int strcmp(const char *a, const char *b);

void poweroff(void) __attribute__((noreturn));
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* Test result helpers: print "PASS"/"FAIL" lines parsed by the runner. */
extern int test_failures;
#define CHECK(cond, ...) do {                                   \
        if (!(cond)) {                                          \
            test_failures++;                                    \
            printf("CHECK FAILED %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)
void test_done(void) __attribute__((noreturn));

/* ---- CPU ---- */
struct exc_frame {
    uint16_t sr;
    uint32_t pc;
    uint16_t fv;           /* format << 12 | vector offset */
} __attribute__((packed));

#define EXC_VECNUM(f)   (((f)->fv & 0x0fff) >> 2)
#define EXC_FORMAT(f)   ((f)->fv >> 12)
typedef void (*exc_handler_t)(struct exc_frame *f, uint32_t *regs);
void set_handler(int vec, exc_handler_t h);
extern volatile uint32_t exc_count[256];

static inline uint16_t get_sr(void)
{
    uint16_t sr;
    __asm__ volatile("movew %%sr,%0" : "=d"(sr));
    return sr;
}
static inline void set_sr(uint16_t sr)
{
    __asm__ volatile("movew %0,%%sr" : : "d"(sr) : "memory");
}
static inline int set_ipl(int ipl)
{
    uint16_t sr = get_sr();
    set_sr((sr & ~0x0700) | (ipl << 8));
    return (sr >> 8) & 7;
}
/* stop with the given IPL: sleeps until an interrupt > ipl arrives */
#define cpu_stop(ipl) __asm__ volatile("stop %0" : : "i"(0x2000 | ((ipl) << 8)) : "memory")

static inline int tas(volatile uint8_t *p)  /* returns old bit 7 set? */
{
    uint8_t r;
    __asm__ volatile("tas %1; smi %0" : "=d"(r), "+m"(*p) : : "cc", "memory");
    return r != 0;
}
static inline uint32_t cas32(volatile uint32_t *p, uint32_t cmp, uint32_t new)
{
    __asm__ volatile("casl %0,%2,%1" : "+d"(cmp), "+m"(*p) : "d"(new) : "cc", "memory");
    return cmp;     /* old value */
}
static inline uint32_t atomic_add_return(volatile uint32_t *p, uint32_t v)
{
    uint32_t o = *p, prev;
    while ((prev = cas32(p, o, o + v)) != o) {
        o = prev;
    }
    return o + v;
}
static inline void atomic_inc(volatile uint32_t *p)
{
    atomic_add_return(p, 1);
}

typedef struct { volatile uint8_t v; } spinlock_t;
static inline void spin_lock(spinlock_t *l)
{
    while (tas(&l->v)) {
        while (l->v) {
        }
    }
}
static inline void spin_unlock(spinlock_t *l)
{
    __asm__ volatile("" ::: "memory");
    l->v = 0;
}

/* Per-CPU entry point, called by crt0 on every CPU. */
void cpu_main(int cpu);
/* Provided by each test: */
int main(void);
void secondary_main(int cpu) __attribute__((weak));
extern void _secondary_start(void);
extern char _end[], _vectors[], _stacks[];

#endif
