# MP pmap performance (NetBSD/virt68k GENERIC.MP)

The MP pmap of `sys/arch/m68k/m68k/pmap_motorola.c` was first written for correctness. Every
entry point took the kernel lock, and every TLB invalidation became a synchronous IPI broadcast
to all the running CPUs, including the one in each `pmap_enter()`, so on every page fault. This
work keeps the architecture's rules (an IPI and a local `PFLUSH` on each target, with no
paravirtual help from QEMU) and removes almost all of that cost. UP kernels are unchanged: the
`.text` of `pmap_motorola.o`, `trap.o` and `virtsmp.o` in GENERIC is byte for byte the same.

## Tools

- `vmstat -e`, group `pmap cpuN`, has per-CPU counters:
  - shootdowns, attributed to the entry point in progress (enter, remove, protect, page_protect,
    clearbit, kremove, other);
  - full flushes, IPIs sent, invalidations that needed no IPI, invalidations merged into a batch,
    and spurious faults fixed.
- lockstat(4) works in MP kernels. `<machine/cpu_counter.h>` reads the virt-smp nanosecond
  counter, and `GENERIC.MP` has `pseudo-device lockstat`.
- `scripts/pmap-bench.sh` (runs in the guest, with `scripts/pmap-bench.c`) runs N parallel copies
  of these tests and prints real, user and sys time plus the counter deltas:

  | test | what each copy does |
  |---|---|
  | fork | 300 × fork() and exit |
  | exec | 150 × vfork() and exec of /usr/bin/true |
  | mmap | 20 × mmap 8 MB, touch, munmap |
  | mprot | 2 threads, 20000 × mprotect RO/RW of a page while the other thread reads it |
  | fault | 4 threads × 5 × mmap, touch, munmap of 4 MB |
  | cc | `make -j4` of 16 small C files |

- `scripts/tlbtest.c` is a TLB coherence test for the remote side. Reader threads are bound to
  the other CPUs and keep the old translation hot in their ATC. After `mprotect(PROT_READ)` a
  write must fault. After `munmap` a read must fault. After `mmap(MAP_FIXED)` a read must see the
  new page. After `fork`, a thread's writes must not reach the child.
- `scripts/netbsd-run.py --monitor SOCK` gives QEMU a monitor socket. On a timeout the script
  sends ^T (wait channel), enters ddb with the gftty(4) console magic `+++++` and runs the
  `--ddb` commands, then dumps every vCPU's PC and a symbolized stack scan (`scripts/qemu-regs.py`).

## Changes, in order

1. **Nothing to shoot down when nothing can be stale.** A mapping that is entered, or only gains
   rights, gets a local flush only. Another CPU can only hold a stale invalid or read-only ATC
   entry, and the fault it takes is fixed by a local flush:
   - `pmap_fault_fixup()` in `trap()` handles kernel addresses;
   - `pmap_enter()`, through `uvm_fault()`, handles user addresses.
2. **Targeted shootdowns.**
   - `pm_cpus` is the set of CPUs that have a user pmap loaded.
   - A CPU keeps its last user pmap loaded (`ci_pmap`) while it runs kernel lwps. Going back to
     the same process then needs neither a URP reload nor a `pflusha`.
   - A user pmap is shot down only on those CPUs. For a single-threaded process that is usually
     none.
   - A destroyed pmap is dropped from the CPUs that still hold it.
3. **Batching and concurrency.**
   - `pmap_remove()` and the other entry points batch their remote invalidations: up to 16 VAs,
     then a full flush, with one IPI round per call.
   - Each CPU has its own request slot, so several shootdowns can be in flight at once.
   - Then: a new user PT page is made cache-inhibited directly (it had no other mapping), and the
     PT pages freed during a batch are freed after its one shootdown. This removed most of the
     remaining kernel-pmap broadcasts.
4. **No kernel lock.**
   - Locks: `pm_lock` (an adaptive mutex) per user pmap, `pmap_klock` (a recursive spin lock) for
     the kernel pmap and for the kernel mappings of user PT pages, and `pmap_pv_slock` for the pv
     lists. The order is pm_lock, pmap_klock, then the pv lock.
   - The pv operations (`pmap_page_protect`, `pmap_clear_*`, `pmap_is_*`) hold the pv lock and
     only *try* to lock each mapping's pmap, backing off on failure.
   - Nothing sleeps with these locks held. `pmap_enter()` allocates without waiting, retries after
     `uvm_wait()`, and allocates its pv entry before locking.
   - Freed PT pages, segment tables and pv entries are released at the end of the operation.
   - User PT pages have no UVM object, because a caller of the pmap may hold `uvm_kernel_object`'s
     lock.
   - PTE bits are updated with CAS, because other CPUs' MMUs set U and M at any time.
   - pv entries come from a pool cache.

   A first version with a global rwlock (single-pmap operations as readers, pv operations as
   writers) was *slower* than the kernel lock. `pmap_clear_reference()` runs on every page
   activation, and each writer drains all the readers. The trylock scheme replaced it.

## Results

8 CPUs (MTTCG on a 12-core host), 768 MB of RAM, our NetBSD 11.0 MP live image. Each cell is
real / sys time in seconds.

| test | copies | baseline | 2: targeted | 3: batched | 3b: PT pages | 4: no kernel lock |
|---|---|---|---|---|---|---|
| fork | 1 | 6.08 / 6.96 | 1.31 / 1.07 | 1.28 / 1.02 | | **1.02 / 0.65** |
| fork | 8 | 37.92 / 293.2 | 8.25 / 59.7 | 8.61 / 62.0 | 7.82 / 55.0 | **2.44 / 11.2** |
| exec | 1 | 6.85 / 6.35 | 1.78 / 1.14 | 1.76 / 0.95 | | **1.61 / 1.12** |
| exec | 8 | 55.11 / 414.6 | 6.98 / 43.9 | 7.30 / 46.7 | 6.98 / 43.8 | **3.16 / 12.9** |
| mmap | 8 | 20.49 / 152.1 | 2.68 / 14.4 | 2.88 / 15.7 | 2.85 / 14.4 | **1.36 / 3.7** |
| fault | 8 | 10.40 / 80.5 | 1.89 / 9.9 | 1.71 / 9.6 | 1.64 / 9.2 | **0.91 / 3.4** |
| cc | 1 | 12.12 / 9.91 | 4.92 / 2.49 | 4.73 / 2.27 | | **4.49 / 1.83** |
| cc | 8 | 34.37 / 117.8 | 10.13 / 43.2 | 10.12 / 43.8 | 11.62 / 56.1 | **5.28 / 9.7** |
| soak.sh 3 8 | | 100.1 / 632.7 | 32.7 / 165.2 | 31.0 / 161.7 | 20.9 / 86.7 | **16.6 / 51.9** |

(The baseline mprot test used 2000 iterations instead of 20000, so its row is not comparable and
is left out.)

- **Self-hosted GENERIC.MP build** (`make -j8` in the guest, `scripts/selfhost-kernel.sh`):
  baseline 770 s real, 2800 s user, 3085 s sys. With step 4: **426 s real, 2077 s user, 1097 s sys**. The build used 520k
  shootdown IPIs, against 7.0M invalidations that needed none.
- **Correctness**, at every step:
  - tlbtest (7 reader threads, 300 to 1000 rounds of each test): 0 failures;
  - soak.sh: SOAK PASS;
  - the paging stress (8 × 95 MB on 768 MB with 128 MB of swap): 8/8;
  - GENERIC.MP.DEBUG (DIAGNOSTIC + LOCKDEBUG): the whole suite passes.
- **The scaling is now reasonable.** fork with 8 copies takes 2.4× the time of one copy for 8×
  the work; the baseline took 6.2×. The cc test does 8× the work in 1.2× the time.

## Bugs found on the way (fixed)

- Lock-order inversions between `pmap_klock` and the pool lock. A pool that grows calls the
  allocator with its lock held (`PR_NOWAIT`), and the allocator needs `pmap_klock` through
  `pmap_kenter_pa`. Hence pv entries are allocated before locking and freed after.
- A sleep deadlock with the first rwlock design:
  - a reader holding `pmap_main_lock` waited for `uvm_kernel_object`'s lock (user PT page
    allocation);
  - that lock's holder was in `uvm_fault()` and wanted the pmap lock.

  Found with ddb (`show all locks/t`). Hence the PT pages have no object.

## What remains

- The kernel pmap is still behind one spin lock, and its shootdowns are still broadcasts (a
  kernel translation may be in any CPU's ATC).
- Device interrupts all go to CPU 0.
- Most of the remaining sys time is outside the pmap: `bufcache_lock`, and the rwlock of the
  soak's vnode/map at the top of lockstat.
