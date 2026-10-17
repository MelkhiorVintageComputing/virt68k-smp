/* t1: walk and check the bootinfo list. */
#include "lib.h"

int main(void)
{
    const struct bi_record *r;

    printf("_end=%p bootinfo=%p\n", _end, bootinfo_first());
    for (r = bootinfo_first(); r; r = bootinfo_next(r)) {
        printf("  tag %04x size %2u:", r->tag, r->size);
        for (unsigned i = 0; i < (r->size - 4u) / 4 && i < 4; i++) {
            printf(" %08lx", (unsigned long)r->data[i]);
        }
        printf("\n");
        if (r->size < 4 || r->size > 1024) {
            panic("bad bootinfo record size %u", r->size);
        }
    }
    printf("qemu %lu.%lu.%lu, mem %luMiB, cpu %lu, %d records\n",
           (unsigned long)bootinfo.qemu_version >> 24,
           (unsigned long)(bootinfo.qemu_version >> 16) & 0xff,
           (unsigned long)(bootinfo.qemu_version >> 8) & 0xff,
           (unsigned long)bootinfo.mem_size >> 20,
           (unsigned long)bootinfo.cputype, bootinfo.nrecords);
    CHECK(bootinfo.machtype == 14, "machtype %lu", (unsigned long)bootinfo.machtype);
    CHECK(bootinfo.mem_size >= 16u << 20, "mem");
    CHECK(bootinfo.pic_base == 0xff000000, "pic");
    CHECK(bootinfo.rtc_base == 0xff006000, "rtc");
    CHECK(bootinfo.tty_base == 0xff008000, "tty");
    CHECK(bootinfo.ctrl_base == 0xff009000, "ctrl");
    CHECK(bootinfo.virtio_base == 0xff010000, "virtio");
    if (bootinfo.smp_base) {
        printf("virt-smp at %p irq %lu\n", (void *)bootinfo.smp_base,
               (unsigned long)bootinfo.smp_irq);
    }
    return 0;
}
