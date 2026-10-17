# virt68k-smp

This project adds SMP to QEMU's m68k `virt` machine (MTTCG, one host thread per vCPU) and adds
MULTIPROCESSOR support to NetBSD/virt68k.

Targets: QEMU 11.1.2 and NetBSD 11.0.

## Layout

| Path | Content |
|------|---------|
| `Inputs/` | Pristine, add-only originals: the QEMU git tree at v11.1.2, the signed QEMU tarball, and the NetBSD 11.0 release and source sets. Read-only; only `Inputs/MANIFEST` is tracked. |
| `work/qemu` | QEMU work tree, branch `virt-smp` (on top of v11.1.2). |
| `work/qemu-stock` | Pristine v11.1.2 worktree, kept for comparison runs. |
| `work/netbsd-src` | NetBSD 11.0 sources (from the source sets), branch `virt68k-mp`. |
| `work/{obj,tools,destdir,release}` | NetBSD build.sh output. |
| `images/` | Kernels and disk images we produce. |
| `doc/virt-smp-spec.md` | Specification of the paravirtual SMP controller (QEMU's copy: `docs/specs/virt-smp.rst`). |
| `doc/baseline-notes.md` | Notes on the stock QEMU virt machine and its MTTCG readiness. |
| `baremetal/` | Freestanding UP (`t*`) and SMP (`s*`) test programs. |
| `scripts/` | `build-qemu.sh`, `run-baremetal.sh`. |
| `patches/qemu/` | `git format-patch` export of the QEMU series. |

## Quick start

```sh
scripts/build-qemu.sh                                     # builds work/qemu/build/qemu-system-m68k
scripts/run-baremetal.sh                                  # UP tests, -smp 1, single-threaded TCG
scripts/run-baremetal.sh -s "1 2 4 8 16" -t "single multi"   # full matrix
QEMU=work/qemu-stock/build/qemu-system-m68k scripts/run-baremetal.sh 't*'   # stock QEMU
```

Each test prints `PASS` or `FAIL` and powers the machine off through virt-ctrl. Logs are written to
`baremetal/build/logs/`.

## QEMU series (patches/qemu)

1. target/m68k: set the M bit on the first write to a page loaded by a read. This is a pre-existing UP
   bug, found by `t4-mmu`.
2. target/m68k: set table descriptor U/M bits atomically. This is an MTTCG race; `s6-mmu` catches it.
3. target/m68k: enable MTTCG.
4. standard-headers: add `BI_VIRT_SMP_BASE`. This is a placeholder: the Linux uapi header has to change first.
5. hw/misc: add the virt-smp device.
6. hw/m68k/virt: support up to 16 CPUs. With `-smp 1` the machine is unchanged.
