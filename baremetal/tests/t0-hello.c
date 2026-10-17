/* t0: console output and poweroff. */
#include "lib.h"

int main(void)
{
    printf("hello from bare-metal m68k\n");
    CHECK(bootinfo.tty_base != 0, "no tty in bootinfo");
    return 0;
}
