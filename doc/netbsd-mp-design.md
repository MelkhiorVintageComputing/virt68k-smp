# NetBSD/virt68k MULTIPROCESSOR: design

The target is NetBSD 11.0 on QEMU `virt` with virt-smp (doc/virt-smp-spec.md). The work is on branch
`virt68k-mp` of `work/netbsd-src`.

## Constraints

- The MP patches are based on the `PG_W` fix (patches/netbsd 0001–0002, doc/pgw-ur.md). On an
  MP 68040 board U0/U1 drive the snoop-control signals, so the pmap's software wired bit must not
  be U0.

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
- Memory ordering. The MC68040 is *not* fully in program order. It completes reads in order and
  writes in order, but "a memory write access [is] potentially delayed for many clocks while
  allowing read accesses generated by later instructions to complete … reordering only occurs
  with writes relative to reads" (M68040UM §7.7, p. 7-43). That is the x86 TSO model, so:
  - `membar_sync` and its deprecated alias `membar_enter` (store-before-load) execute `NOP`,
    which "freezes instruction execution until all pending bus cycles have completed"
    (p. 7-44). Locked accesses (TAS/CAS) also wait for pending writes (p. 4-7).
  - `membar_acquire`, `membar_release`, `membar_producer`, `membar_consumer` and `membar_exit`
    need no instruction: the function call is the compiler barrier.
  - The code is in `common/lib/libc/arch/m68k/atomic/membar_ops.S`, and replaces the all-nop
    `membar_ops_nop.c` on m68k. The plain-68000 branch keeps the nop version.
  - Under QEMU (TCG_MO_ALL) the difference is invisible. It matters on real silicon.
  - Note: the earlier version of this document said that the 040 keeps accesses in program
    order. That was wrong.
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
  - Kernel-pmap shootdowns are broadcasts, and the kernel pmap has one spin lock (see
    doc/pmap-perf.md for the user pmaps, which now have targeted shootdowns and their own locks).
  - Device interrupts on CPU 0 only.
  - Modules are built without MULTIPROCESSOR (curlwp is unresolved), so they cannot be loaded
    into GENERIC.MP.
- Soak (`scripts/soak.sh 6 8`, 8 CPUs, our own release image with the fixed libc atomics):
  6 rounds of parallel `make -j8` plus 8 concurrent file copies, all checksums correct, SOAK PASS.
  The system time is very high (1017 s sys vs 151 s user for 179 s wall). The suspects are the
  broadcast TLB shootdowns and kernel-lock pmap serialization; this is the next optimisation
  target (per-pmap CPU sets for targeted shootdown, batching in pmap_remove, a finer pmap lock).

## Self-hosting (2026-10-17)

- The sources (`sys`, `common`) go on a second virtio disk (`images/src-disk.img`, 2 GB FFS made
  with nbmakefs), which shows up as ld1 and is mounted on /usr/src. The procedure is in
  `scripts/selfhost-kernel.sh`; the kernel is sent back to the host with `nc` over user networking
  (`scripts/recv-file.py`).
- GENERIC.MP builds natively in the 8-CPU guest with `make -j8` (768 MB RAM) in 770 s real,
  2800 s user, 3085 s sys. The resulting kernel boots with 8 CPUs and passes the paging stress and
  the network test.
- This found an MP bug: vioif(4) attached before the secondary CPUs, so its per-CPU workqueue
  was sized for 1 CPU, and `workqueue_wait()` (from `ether_ioctl()`, e.g. when dhcpcd brings the
  interface up) walked past the allocation. It showed up as a corrupted mutex owner (0x84) without
  LOCKDEBUG, and as "uninitialized lock" with GENERIC.MP.DEBUG. Fix: virtsmp(4) and the CPUs are
  now attached first on mainbus.

## pmap performance (2026-10-18)

The kernel lock and the broadcast shootdowns are gone from the pmap. The changes are targeted
and batched shootdowns, lazy pmap activation, per-pmap locks, and trylock pv walks. On 8 CPUs the
soak takes 16.6 s instead of 100 s (sys 52 s instead of 633 s), and the self-hosted kernel build
takes 426 s instead of 770 s. The details, tools and measurements are in doc/pmap-perf.md.
