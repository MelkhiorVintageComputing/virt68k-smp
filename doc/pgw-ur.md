# m68k: software wired bit (PG_W) moved from U0 to UR

This is a standalone patch set against the pristine NetBSD 11.0 sources, independent of the
MULTIPROCESSOR work: `patches/netbsd-pgw-ur/`, from branch `pgw-ur` of `work/netbsd-pgw` (based
on 2ade45572). The background investigation is in `pg-w-u0-findings.md` (repo root, not
committed).

## Why

On the 68040/060, `PG_W` was `__BIT(8)`, which is U0, a "user page attribute" bit.
- The CPU keeps U0 in the ATC and drives it on the UPA0 pin for the bus cycles to the page.
- Motorola recommends wiring UPA0/UPA1 to the snoop-control inputs on MP boards, and the atari
  Milan uses U0 for the PLX9080's big-endian control.
- So every wired page (all `pmap_kenter_pa()` kernel pages, and `mlock`ed user pages) had a
  hardware side effect.
- `<m68k/mmu_40.h>` itself says U0/U1 must not be used as software bits.

## Patches

1. **`m68k: use a user-reserved PTE bit for the software wired flag`**
   - `PG_W` becomes `__BIT(11)`: `PTE40_UR` on the 040/060, and an unused page-address bit on the
     851/030, as bit 8 was.
   - `CTASSERT(PG_W < __BIT(PG_SHIFT))` checks that it is below the frame (`PGSHIFT` is 12 or 13
     on every port).
   - Comments are updated: the "wiring-only change needs no TLB flush" shortcuts are now correct,
     because UR is not kept in the ATC, whereas U0 was.
   - Generated code: only the constant changes (0x100 → 0x800 in 12 instructions of
     `pmap_motorola.o`).
2. **`atari: name the U0 page attribute used on the Milan`** (cosmetic). `0x100` becomes
   `PTE40_U0`; no functional change.

## Tests (virt68k GENERIC, UP, stock QEMU 11.1.2, stock 11.0 image, 512 MB)

| | unmodified | patched |
|---|---|---|
| PTE scan, kernel tree (`scripts/pte-check.sh`) | ~7090 wired pages with U0, none with bit 11 | none with U0/U1, ~7090 with bit 11 |
| PTE scan, user tree with 100 `mlock`ed + 50 unwired pages | the 100 wired pages have U0 | the 100 wired pages have bit 11, U0/U1 never set |
| `wiretest` (3000 `mlock`/`munlock`, 3 writer threads) | PASS | PASS |
| `vmstat -s` "pages wired" with 200 pages locked / after | 201 / 1 | 200 / 0 |
| ATF libc sys (mlock, mmap, mprotect, munmap, msync) | 28 pass, 1 xfail, 3 skip | identical |
| ATF libpthread | 172 pass, 8 xfail, 1 skip | identical |
| `soak.sh 3 2` | PASS, 45.9 s | PASS, 45.6 s |
| paging stress 4 × 60 MB | 4/4 | 4/4 |
| `pmap-bench.sh 1` | — | same times |
| `crash` on the live kernel | OK | OK |

**Compile-only builds** (both patches): amiga GENERIC (8K pages, 030/040/060), atari MILAN-PCIIDE
and ATARITT, hp300 GENERIC (HP MMU and 851/030), mac68k GENERIC and mvme68k GENERIC.

**Not testable here:** the effect on real UPA pins, the 851/030 MMU (QEMU does not emulate it),
and 8K pages at run time.

## Tools

- `scripts/pte-scan.py MONITOR_SOCKET [-u] [-v]`: walks the guest's 68040 page tables from the
  QEMU monitor (`p $srp`/`$urp`, `xp`) and counts the resident pages with U0, U1 or UR set.
- `scripts/pte-check.sh KERNEL [NLOCK NFREE]`: boots the kernel UP on stock QEMU, runs
  `scripts/mlock-hold.c` in the guest, and scans both trees.
- `scripts/wiretest.c`: an `mlock`/`munlock` stress with writer threads; checks the data after
  `msync` and the wired count.

## Merged into the MP branch (2026-10-26)

`pgw-ur` is merged into `virt68k-mp`, and the merge was clean. The MP `pmap_unwire()` keeps its
atomic clear, which now clears bit 11. `patches/netbsd/` now carries the two patches as 0009 and
0010. The series applies to the pristine tree and reproduces the branch exactly.

The merged GENERIC.MP passes these tests:
- page table scan: no U0/U1 in the kernel tree, and bit 11 on the wired pages;
- on 8 CPUs: tlbtest, wiretest, `soak.sh 3 8` (16.4 s, as before the merge) and the paging
  stress;
- the same tests on GENERIC.MP.DEBUG (LOCKDEBUG).
