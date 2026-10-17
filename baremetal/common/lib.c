/* Minimal support library for bare-metal tests on QEMU m68k "virt". */
#include "lib.h"

struct bootinfo bootinfo;
int test_failures;
volatile uint32_t exc_count[256];
static exc_handler_t handlers[256];
static spinlock_t print_lock;

/* ---- bootinfo ---- */
const struct bi_record *bootinfo_first(void)
{
    return (const struct bi_record *)(((uintptr_t)_end + 1) & ~1u);
}

const struct bi_record *bootinfo_next(const struct bi_record *r)
{
    if (r->tag == BI_LAST) {
        return NULL;
    }
    return (const struct bi_record *)((const char *)r + r->size);
}

void bootinfo_parse(void)
{
    const struct bi_record *r;

    for (r = bootinfo_first(); r; r = bootinfo_next(r)) {
        bootinfo.nrecords++;
        switch (r->tag) {
        case BI_MACHTYPE:  bootinfo.machtype = r->data[0]; break;
        case BI_CPUTYPE:   bootinfo.cputype = r->data[0]; break;
        case BI_FPUTYPE:   bootinfo.fputype = r->data[0]; break;
        case BI_MMUTYPE:   bootinfo.mmutype = r->data[0]; break;
        case BI_MEMCHUNK:
            bootinfo.mem_base = r->data[0];
            bootinfo.mem_size = r->data[1];
            break;
        case BI_COMMAND_LINE: bootinfo.cmdline = (const char *)r->data; break;
        case BI_VIRT_QEMU_VERSION: bootinfo.qemu_version = r->data[0]; break;
#define BASE_IRQ(tag, f) \
        case tag: bootinfo.f##_base = r->data[0]; bootinfo.f##_irq = r->data[1]; break;
        BASE_IRQ(BI_VIRT_GF_PIC_BASE, pic)
        BASE_IRQ(BI_VIRT_GF_RTC_BASE, rtc)
        BASE_IRQ(BI_VIRT_GF_TTY_BASE, tty)
        BASE_IRQ(BI_VIRT_VIRTIO_BASE, virtio)
        BASE_IRQ(BI_VIRT_CTRL_BASE, ctrl)
        BASE_IRQ(BI_VIRT_SMP_BASE, smp)
        }
    }
}

/* ---- devices ---- */
void pic_enable(uint32_t irq)
{
    mmio_write32(bootinfo.pic_base + PIC_OF_IRQ(irq) * 0x1000 + PIC_REG_ENABLE,
                 1u << LINE_OF_IRQ(irq));
}

void pic_disable(uint32_t irq)
{
    mmio_write32(bootinfo.pic_base + PIC_OF_IRQ(irq) * 0x1000 + PIC_REG_DISABLE,
                 1u << LINE_OF_IRQ(irq));
}

uint64_t rtc_ns(uintptr_t rtc)
{
    uint32_t lo = mmio_read32(rtc + RTC_TIME_LOW);
    uint32_t hi = mmio_read32(rtc + RTC_TIME_HIGH);
    return ((uint64_t)hi << 32) | lo;
}

void rtc_set_alarm(uintptr_t rtc, uint64_t abs_ns)
{
    mmio_write32(rtc + RTC_ALARM_HIGH, abs_ns >> 32);
    mmio_write32(rtc + RTC_ALARM_LOW, (uint32_t)abs_ns);   /* arms */
}

void delay_ns(uint64_t ns)
{
    uint64_t end = rtc_ns(bootinfo.rtc_base) + ns;
    while (rtc_ns(bootinfo.rtc_base) < end) {
    }
}

/* ---- console ---- */
#define DEFAULT_TTY 0xff008000u

void putchar_raw(int c)
{
    uintptr_t tty = bootinfo.tty_base ? bootinfo.tty_base : DEFAULT_TTY;
    if (c == '\n') {
        mmio_write32(tty, '\r');
    }
    mmio_write32(tty, (uint8_t)c);
}

void puts_raw(const char *s)
{
    while (*s) {
        putchar_raw(*s++);
    }
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    while (n--) {
        *p++ = c;
    }
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    while (n--) {
        *p++ = *q++;
    }
    return d;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++, b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

struct outbuf {
    char *buf;
    size_t n, len;
};

static void out(struct outbuf *o, char c)
{
    if (o->len + 1 < o->n) {
        o->buf[o->len] = c;
    }
    o->len++;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct outbuf o = { buf, n, 0 };
    char tmp[24];

    for (; *fmt; fmt++) {
        int width = 0, zero = 0, lng = 0, left = 0, i, len;
        unsigned long long v;
        const char *s;
        int base, neg;

        if (*fmt != '%') {
            out(&o, *fmt);
            continue;
        }
        fmt++;
        if (*fmt == '-') {
            left = 1, fmt++;
        }
        if (*fmt == '0') {
            zero = 1, fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') {
            lng++, fmt++;
        }
        switch (*fmt) {
        case 'c':
            out(&o, va_arg(ap, int));
            continue;
        case 's':
            s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            for (len = 0; s[len]; len++) {
            }
            if (!left) {
                for (i = len; i < width; i++) {
                    out(&o, ' ');
                }
            }
            while (*s) {
                out(&o, *s++);
            }
            if (left) {
                for (i = len; i < width; i++) {
                    out(&o, ' ');
                }
            }
            continue;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'p':
            base = (*fmt == 'x' || *fmt == 'X' || *fmt == 'p') ? 16 : 10;
            neg = 0;
            if (*fmt == 'p') {
                v = (uintptr_t)va_arg(ap, void *);
                out(&o, '0'), out(&o, 'x');
                width = 8, zero = 1;
            } else if (*fmt == 'd' || *fmt == 'i') {
                long long sv = lng >= 2 ? va_arg(ap, long long)
                             : lng ? va_arg(ap, long) : va_arg(ap, int);
                if (sv < 0) {
                    neg = 1, sv = -sv;
                }
                v = sv;
            } else {
                v = lng >= 2 ? va_arg(ap, unsigned long long)
                  : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            }
            len = 0;
            do {
                unsigned d = v % base;
                tmp[len++] = d < 10 ? '0' + d : 'a' + d - 10;
                v /= base;
            } while (v);
            if (neg) {
                if (zero) {
                    out(&o, '-');
                } else {
                    tmp[len++] = '-';
                }
                width--;
            }
            for (i = len; i < width; i++) {
                out(&o, zero ? '0' : ' ');
            }
            while (len) {
                out(&o, tmp[--len]);
            }
            continue;
        case '%':
            out(&o, '%');
            continue;
        default:
            out(&o, '%');
            out(&o, *fmt);
            continue;
        }
    }
    if (n) {
        buf[o.len < n ? o.len : n - 1] = 0;
    }
    return o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* printf is serialized across CPUs, and masks interrupts while holding
 * the lock so an interrupt handler on the same CPU cannot deadlock. */
int printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int r, ipl;

    va_start(ap, fmt);
    r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ipl = set_ipl(7);
    spin_lock(&print_lock);
    puts_raw(buf);
    spin_unlock(&print_lock);
    set_ipl(ipl);
    return r;
}

#define DEFAULT_CTRL 0xff009000u
#define CTRL_CMD     0x04
#define CTRL_HALT    2
#define CTRL_PANIC   3

void poweroff(void)
{
    uintptr_t ctrl = bootinfo.ctrl_base ? bootinfo.ctrl_base : DEFAULT_CTRL;
    mmio_write32(ctrl + CTRL_CMD, CTRL_HALT);
    for (;;) {
        cpu_stop(7);
    }
}

void panic(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    uintptr_t ctrl = bootinfo.ctrl_base ? bootinfo.ctrl_base : DEFAULT_CTRL;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    puts_raw("PANIC: ");
    puts_raw(buf);
    puts_raw("\nFAIL\n");
    mmio_write32(ctrl + CTRL_CMD, CTRL_PANIC);
    for (;;) {
        cpu_stop(7);
    }
}

void test_done(void)
{
    if (test_failures) {
        printf("FAIL (%d failures)\n", test_failures);
    } else {
        printf("PASS\n");
    }
    poweroff();
}

/* ---- exceptions ---- */
void set_handler(int vec, exc_handler_t h)
{
    handlers[vec] = h;
}

void exc_dispatch(struct exc_frame *f, uint32_t *regs)
{
    int vec = EXC_VECNUM(f);

    exc_count[vec]++;
    if (handlers[vec]) {
        handlers[vec](f, regs);
        return;
    }
    /* Spurious autovector interrupts are tolerated. */
    if (vec == 24) {
        return;
    }
    panic("unexpected exception vector %d (format %d) pc=%p sr=%04x",
          vec, EXC_FORMAT(f), (void *)f->pc, f->sr);
}

void cpu_main(int cpu)
{
    if (cpu == 0) {
        bootinfo_parse();
        main();
        test_done();
    }
    if (secondary_main) {
        secondary_main(cpu);
    }
    for (;;) {
        cpu_stop(7);
    }
}
