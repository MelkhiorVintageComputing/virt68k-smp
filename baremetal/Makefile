# Bare-metal tests for QEMU m68k "virt" (UP and SMP).
CROSS   ?= m68k-linux-gnu-
CC      := $(CROSS)gcc
CPU     ?= 68040
CFLAGS  := -m$(CPU) -O2 -g -Wall -Wextra -Wno-unused-parameter \
           -ffreestanding -fno-builtin -fno-pic -fno-stack-protector \
           -fno-asynchronous-unwind-tables -Icommon
LDFLAGS := -nostdlib -static -no-pie -Wl,-T,common/link.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments
LIBS    := -lgcc

B       := build
COMMON  := $(B)/crt0.o $(B)/lib.o
TESTS   := $(patsubst tests/%.c,%,$(wildcard tests/*.c))
ELFS    := $(addprefix $(B)/,$(addsuffix .elf,$(TESTS)))

all: $(ELFS)

$(B):
	mkdir -p $@
$(B)/%.o: common/%.S $(wildcard common/*.h) | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/%.o: common/%.c $(wildcard common/*.h) | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/%.t.o: tests/%.c $(wildcard common/*.h) | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<
$(B)/%.elf: $(B)/%.t.o $(COMMON) common/link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(COMMON) $< $(LIBS)

clean:
	rm -rf $(B)
.PHONY: all clean
.PRECIOUS: $(B)/%.t.o
