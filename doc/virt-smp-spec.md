# virt-smp: paravirtual SMP controller for the QEMU m68k `virt` machine

Version 1. This is the single reference that the QEMU device (`hw/misc/virt_smp.c`), the
bare-metal tests (`baremetal/`) and the NetBSD/virt68k MULTIPROCESSOR code are all written against.

No real 680x0 SMP machine exists, so this device is invented. It provides:

- CPU identification (`ID`) and enumeration (`NCPUS`),
- secondary CPU start and stop,
- inter-processor interrupts (IPIs) with 16 reason bits,
- one timer per CPU, plus a global 64-bit nanosecond counter,
- per-CPU interrupt delivery, including routing of the Goldfish PIC outputs to a chosen CPU.

## Discovery

The device exists only when the machine is started with `-smp N` and N > 1. With `-smp 1` the
machine is identical to stock QEMU (no device, no bootinfo record, the old `m68k-irq-controller` path).

Bootinfo record `BI_VIRT_SMP_BASE` = **0x8007** (0x8006 is reserved for BI_VIRT_RNG_SEED):

| word | content                          |
|------|----------------------------------|
| 0    | MMIO base address (0xff00a000)   |
| 1    | number of CPUs (N)               |

## Programming model

- The MMIO window is 4 KiB at `0xff00a000`. All registers are 32 bits wide, native (big) endian,
  and must be accessed with aligned 32-bit loads and stores.
- CPUs are numbered 0..N-1, with N ≤ 16. CPU 0 is the boot CPU. QEMU's `cpu_index` is the CPU number.
- In the **banked** window (0x000-0x0ff), each access hits the registers of the CPU that performs it.
  Accesses that do not come from a CPU (gdbstub, monitor) are treated as coming from CPU 0.
- Reserved registers read as 0 and ignore writes. Unaligned or non-32-bit accesses are logged as guest
  errors and otherwise treated as reserved.

### Banked registers (the accessing CPU)

| off   | name          | access | description |
|-------|---------------|--------|-------------|
| 0x000 | ID            | RO     | number of the accessing CPU |
| 0x004 | NCPUS         | RO     | N |
| 0x008 | VERSION       | RO     | 1 |
| 0x010 | IPI_PENDING   | RO     | bitmask of pending IPI reasons (bits 15..0) |
| 0x014 | IPI_CLEAR     | W1C    | clear the reasons written as 1 |
| 0x018 | IPI_CONFIG    | RW     | bits 2..0: IPL (0 = IPIs disabled); bits 15..8: vector. Reset: IPL 6, vector 0x40 |
| 0x020 | TIMER_CTRL    | RW     | bit 0: enable, bit 1: periodic. Writing it (re)starts the countdown from TIMER_INTERVAL |
| 0x024 | TIMER_INTERVAL| RW     | countdown in ns (one-shot delay or period). 0 = stopped |
| 0x028 | TIMER_STATUS  | RO/W1C | bit 0: expired (interrupt pending); write 1 to acknowledge |
| 0x02c | TIMER_CONFIG  | RW     | bits 2..0: IPL (0 = timer interrupt disabled); bits 15..8: vector. Reset: IPL 6, vector 0x41 |

Timer semantics: with TIMER_CTRL.enable set, the timer expires TIMER_INTERVAL ns after the write to
TIMER_CTRL (or after the previous expiry, in periodic mode, without drift accumulation). Each expiry
sets TIMER_STATUS.expired. While that bit is set and the IPL is non-zero, an interrupt is asserted on
the owning CPU. Writing TIMER_CTRL with enable=0 stops the timer but does not clear the expired bit.

IPI semantics: an IPI is asserted on a CPU as long as its IPI_PENDING is non-zero and its IPL is non-zero.

### Global registers

| off   | name          | access | description |
|-------|---------------|--------|-------------|
| 0x100 | IPI_SEND      | WO     | bits 31..16: reasons; bits 15..0: target CPU mask. ORs the reasons into IPI_PENDING of each target in a single access, so it is race-free between CPUs. A CPU may target itself |
| 0x110 | DEVIRQ_ROUTE  | RW     | 4 bits per input level L (1..7) at bits [4(L-1)+3 : 4(L-1)]: the CPU that receives the Goldfish PIC #L output (L=1..6) and NMI (L=7). Reset: 0 (all to CPU 0). Values ≥ N are treated as 0 |
| 0x120 | COUNTER_LO    | RO     | global ns counter (QEMU_CLOCK_VIRTUAL), low 32 bits. Reading it latches COUNTER_HI for the accessing CPU |
| 0x124 | COUNTER_HI    | RO     | high 32 bits latched by the last COUNTER_LO read of this CPU |
| 0x200 + 16n | START_PC(n)  | RW | initial PC for CPU n |
| 0x204 + 16n | START_SP(n)  | RW | initial SSP (A7 = ISP) for CPU n |
| 0x208 + 16n | START_ARG(n) | RW | value placed in D0 and A0 when CPU n starts |
| 0x20c + 16n | CPU_CTRL(n)  | RW | read: 0 = stopped, 1 = running. Write 1: start CPU n if it is stopped (ignored otherwise). Write 0: stop CPU n |

CPU 0 cannot be stopped through CPU_CTRL (writing 0 is ignored). Use virt-ctrl to halt or reset the machine.

### Secondary CPU start state

On a start request, CPU n is reset (as by a CPU reset: SR = 0x2700, VBR = 0, MMU off, TT registers
cleared, caches off), then:
`PC = START_PC(n)`, `A7 (ISP) = START_SP(n)`, `D0 = A0 = START_ARG(n)`.

All of CPU n's per-CPU state is also reset: IPI_PENDING = 0, timer stopped and not expired, and the
CONFIG registers back to their reset values.

A stopped CPU executes nothing and takes no interrupts. IPIs sent to it are discarded, and device
interrupts routed to it are not delivered. On a machine reset, every CPU except CPU 0 is stopped and
the device returns to its reset state.

The start is asynchronous: the requesting CPU must poll for a handshake (for example a flag written
by the new CPU) and must not assume the new CPU is running when its store completes. CPU_CTRL(n)
reads 1 as soon as the request has been accepted.

### Interrupt delivery

Each CPU has its own priority logic. Its inputs are:

1. Goldfish PIC #L (L = 1..6), for each L whose DEVIRQ_ROUTE entry selects this CPU: level L, **autovector** (24 + L).
2. NMI (monitor `nmi`), if DEVIRQ_ROUTE level 7 selects this CPU: level 7, autovector 31.
3. IPI: level IPI_CONFIG.IPL, vector IPI_CONFIG.vector.
4. Timer: level TIMER_CONFIG.IPL, vector TIMER_CONFIG.vector.

The CPU is interrupted at the highest asserted level. Among sources at the same level, priority is
IPI, then timer, then PIC/NMI, and that source's vector is used. Delivery is level-triggered: a source
stays asserted until the guest acknowledges it at the device (IPI_CLEAR, TIMER_STATUS, PIC/device
registers).

The default IPL of 6 lets the guest mask IPIs and timer interrupts with an SR IPL ≥ 6. In NetBSD
terms they are blocked by `splsched()`/`splhigh()`. The vectors default to 0x40/0x41, the first user
vectors, so the guest needs no demultiplexing.

## Memory model and atomics (guest-visible guarantees)

- Under MTTCG, vCPUs run in parallel host threads. QEMU uses `TCG_MO_ALL` for m68k, so the guest sees
  sequentially consistent ordering for plain loads and stores (no barrier instructions exist on 680x0).
- TAS, CAS and CAS2 are atomic with respect to all other CPUs. Other read-modify-write instructions
  (`addq` to memory, BSET, BFxxx, ...) are not.
- The 68040 MMU table walk sets the U and M bits with an atomic OR. TLB (ATC) state is per CPU, and
  PFLUSH only affects the executing CPU, so the guest must do TLB shootdowns with IPIs.
- Caches are not modelled (they are coherent), and CINV/CPUSH are no-ops.
