# NetBSD/virt68k MULTIPROCESSOR: design

The target is NetBSD 11.0 on QEMU `virt` with virt-smp (doc/virt-smp-spec.md). The work is on branch
`virt68k-mp` of `work/netbsd-src`.

## Constraints

- `sys/arch/m68k` is shared by every m68k port. All MP changes are behind `#ifdef MULTIPROCESSOR`
  (a `defflag` in `opt_multiprocessor.h`), and only virt68k enables it. Kernels built without
  MULTIPROCESSOR must stay byte-for-byte the same in behaviour.
- Today there is no MP code at all. `curcpu()` is `&cpu_info_store`, and `curlwp`, `curpcb`,
  `astpending`, `intr_depth` and `tmpstk` are globals, used from assembly as well.

## Per-CPU state: curcpu() via VBR

Each CPU has its own vector base register, and NetBSD sets it once per CPU. Each CPU gets a
**vector block**:

```c
struct m68k_vecblk {
	void		*vb_vectors[NVECTORS];	/* what VBR points to */
	struct cpu_info	*vb_ci;			/* at VBR + NVECTORS*4 = VBR + 1024 */
};
```

so

```
curcpu():   movec %vbr,%aN ; movl %aN@(1024),%aN
```

This works on both the 68040 and the 68060 (the 060 has no MSP, so the "spare MSP" trick is not
available). It also costs no MMIO trap and no extra MMU state. CPU 0's block holds the global
`vectab` (vectors.c), which becomes the first member of `vectab_blk` under MULTIPROCESSOR, and its
`vb_ci` = `&cpu_info_store`.

Secondary CPUs copy CPU 0's table into their own block. `vec_set_entry()` updates every block.
locore points VBR at `vectab_blk` (physical = virtual for the kernel) before calling any C code,
so `curcpu()` is valid from the first instruction.

The new per-CPU fields of `struct cpu_info` (MULTIPROCESSOR only):

| field          | replaces global | used from asm as |
|----------------|-----------------|------------------|
| `ci_curlwp`    | `curlwp`        | `CI_CURLWP` (MI `curlwp` becomes `curcpu()->ci_curlwp`) |
| `ci_curpcb`    | `curpcb`        | `CI_CURPCB` (copy.s already has the hook) |
| `ci_astpending`| `astpending`    | `CI_ASTPENDING` |
| `ci_idepth`    | `intr_depth`    | `CI_IDEPTH` (the field already exists) |
| `ci_tmpstk`    | `tmpstk`        | `CI_TMPSTK` (per-CPU scratch stack for cpu_switchto) |
| `ci_self`, `ci_cpuid`, `ci_next`, IPI state | | |

In C, `curpcb`, `astpending` and `intr_depth` become macros for the curcpu() fields. `nofault`
stays global: it is only used for bus probing (autoconf, boot CPU) and the FPU probe.

The asm helpers in `m68k/include/asm.h` are `GETCURCPU(An)`, `GETCURLWP(An)` and
`GETCURPCB(An)`. Without MULTIPROCESSOR they expand to the current global accesses.

## Boot of secondary CPUs

1. `virtsmp` attaches from the `BI_VIRT_SMP_BASE` bootinfo record (mainbus). It attaches `cpu0`
   (the boot CPU, `cpu_info_store`) and `cpu1..N-1`, each with an allocated cpu_info.
2. `cpu_attach` (secondaries): allocate the cpu_info, the vector block, the idle lwp stack (via
   `mi_cpu_attach`), and the tmpstk.
3. `cpu_boot_secondary_processors()` (called from main): for each secondary, set
   START_PC = `cpu_spinup_trampoline` (locore, physical = virtual), START_SP = its boot stack,
   START_ARG = its cpu_info, then CPU_CTRL = 1.
4. The trampoline (MMU off) loads TC/SRP/URP/TT/CACR as CPU 0 did in locore (shared kernel page
   tables), sets VBR to its vector block, switches to the idle lwp stack, and calls `cpu_hatch(ci)`.
5. `cpu_hatch()`: sets up the per-CPU timer (hardclock), marks itself running in `kcpuset_running`,
   and enters `idle_loop()`.

## Interrupts and IPIs

- virt-smp delivers IPIs at IPL 6 (IPL_SCHED), vector 0x40, and the per-CPU timer at IPL 6,
  vector 0x41. Both get dedicated asm stubs (like `intrhand_autovec`) that bump `ci_idepth` and call
  `virtsmp_ipi_intr()` / `virtsmp_clock_intr(frame)`.
- Device interrupts (goldfish-pic) stay on CPU 0 (DEVIRQ_ROUTE = 0).
- IPI reasons: NOP, AST/resched, XCALL, GENERIC (`ipi(9)`), HALT, DDB.
- MI hooks: `cpu_ipi()`, `xc_send_ipi()`, `cpu_need_resched()` (a real function under MP: it sets
  the target's `ci_astpending` and IPIs it if it is remote), `cpu_signotify()`.

## Clocks

- Each CPU runs `hardclock()` from its virt-smp timer (periodic, `1/hz`). CPU 0 also keeps
  timekeeping (`tc_ticktock`).
- Timecounter: the virt-smp COUNTER (ns, 64-bit), which is also better than the dummy counter used
  today on UP.

## pmap (pmap_motorola.c)

- Coarse locking first: a global spin mutex, `pmap_lock`, at IPL_VM, around every pmap operation
  that changes PTEs or pv lists, replacing the bare `splvm()`.
- TLB shootdown: after changing a valid PTE of the kernel pmap, or of a user pmap that is active on
  other CPUs (tracked with a per-pmap `kcpuset` of CPUs that have loaded it in `pmap_activate`),
  send a synchronous xcall that does `TBIS`/`TBIA` on each target. Correctness first; batching later.
- `CADDR1`/`CADDR2` (zero/copy page windows) become per-CPU.
- `pmap_testbit`/`pmap_changebit` on M/U: clearing needs a shootdown too.

## Atomics, locks, barriers

- `__cpu_simple_lock` (TAS) and the CAS-based atomic_ops are already MP-safe on QEMU (MTTCG).
- `membar_*` are nops. That is correct for QEMU's TCG_MO_ALL model, and it is the 680x0 model
  (no barrier instructions; the 040/060 keep accesses in program order).
- mutex stubs read `curlwp`; they move to `GETCURLWP`.

## Steps

| step | content | test |
|------|---------|------|
| N1 | per-CPU plumbing under MULTIPROCESSOR, still 1 CPU | GENERIC.MP boots `-smp 1`, run the regression set |
| N2 | `virtsmp`, `cpu*` attach, virt-smp timecounter | GENERIC.MP boots `-smp 4`, `cpu1..3` attached but not started |
| N3 | secondary spin-up, idle loop, per-CPU hardclock | `hw.ncpu=4`, CPUs idle |
| N4 | IPIs: xcall, ipi(9), resched/AST, halt | `cpuctl list`, parallel workload |
| N5 | pmap locking + TLB shootdown | stress: parallel builds, ATF |
| N6 | cleanup, LOCKDEBUG/DIAGNOSTIC soak | |

## Status (2026-10-17)

- GENERIC.MP boots multi-user with -smp 1, 2, 4 and 8 (MTTCG).
- With 8 CPU-bound processes, the speedup is about 3.7× on 4 CPUs and 5.8× on 8.
- ATF `/usr/tests/lib/libpthread` on 4 CPUs (stock 11.0 userland): 172 passed, 0 failed,
  8 expected failures (known PRs), 1 skipped.
- Bugs found and fixed on the way:
  - **NetBSD**: the void `atomic_{add,sub,and,or,xor,inc,dec}_32` were plain RMW instructions,
    which are not atomic across CPUs (they caused the 4-CPU boot hang via the TLB shootdown
    ack counter). This also affects userland threads, so userland needs a rebuilt libc.
  - **QEMU**: the M bit was not set on a write after a read, and the U/M updates were non-atomic
    under MTTCG.
- Known simplifications:
  - Broadcast TLB shootdown on every invalidation.
  - Kernel-lock pmap.
  - Device interrupts on CPU 0 only.
  - Modules are built without MULTIPROCESSOR (curlwp is unresolved), so they cannot be loaded
    into GENERIC.MP.
