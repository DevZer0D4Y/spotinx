# lkl-switch

**Linux running as a homebrew app inside Horizon OS — and running real,
unmodified Linux binaries (busybox) — on a hacked Nintendo Switch.**

Not L4T. Not a reboot. This is the Linux kernel (6.12) linked *into* an NRO
homebrew, booting and running alongside Horizon OS, plus a from-scratch ELF
loader + syscall bridge that runs an actual aarch64 Linux binary on top of it —
including a real, off-the-shelf static-glibc **busybox**.

As far as I can tell, nobody has published Linux *executing real binaries* from
inside Horizon before. L4T replaces the system at boot; this runs as an app.

```
=== Fase 8: busybox real ===
Rodando: busybox ls /

data  dev  init  root  tmp
```

*(real `busybox ls /` on the Switch, listing the LKL root filesystem)*

> ⚠️ **What this is not:** no GUI, no browser, no GPU, no desktop. The kernel
> has no MMU (`CONFIG_MMU` off) and cannot run graphical software. If you want a
> Linux desktop on your Switch, use [L4T Ubuntu](https://wiki.switchroot.org/).
> This project is a different thing: the Linux kernel as a library inside a
> homebrew, running real binaries — a technical first, and a foundation to build on.

## What works (tested on hardware)

- **Boots Linux 6.12** inside an NRO — full dmesg, on-screen.
- **Runs syscalls** against the live kernel: `uname`, mount `tmpfs`, create/read files.
- **Runs real, unmodified busybox**: a stock static-glibc aarch64 busybox is
  loaded, relocated, its `svc` instructions patched, and executed on top of the
  live LKL kernel. `busybox ls /`, `uname -a`, `echo`, `ls -la` all work —
  full glibc process startup (TLS, pointer guard, exit handlers, IFUNC relocs)
  running under a from-scratch syscall bridge, no CPU emulation.

The executor runs the guest **natively** (it's aarch64, same as the Switch): no
CPU emulation, no exception handler. It rewrites each `svc #0` into a branch to a
trampoline that fully preserves guest state (x0–x18/x30, NZCV/FP, `q0–q31`, and
the guest's `TPIDR_EL0`), swaps to a host stack + thread pointer, and forwards
the Linux syscall ABI into `lkl_syscall()`. The ELF image is mapped with proper
per-segment page permissions via `svcMapProcessCodeMemory` (RX `.text`, RW
`.data`/`.bss`) at one load bias — the correct Horizon W^X pattern (RWX is
forbidden by the kernel). See [DEVLOG.md](DEVLOG.md) for the long bug hunt this
took (process stack + auxv, TLS, pointer-guard, SIMD preservation, the `x0=0`
process-entry bug, and W^X on hardware).

## How it works

```
┌─ NRO (homebrew) ─────────────────────────────────────────┐
│  main.c        boots LKL on a dedicated thread            │
│  nx_host.c     lkl_host_operations via libnx              │
│                (threads, semaphores, timers, console)     │
│  ┌─ LKL (Linux 6.12, ARCH=lkl, no-MMU) ────────────────┐  │
│  │  full kernel: VFS, tmpfs, ext4, TCP/IP stack        │  │
│  └─────────────────────────────────────────────────────┘  │
│  nx_exec.c     ELF loader → svcMapProcessCodeMemory       │
│                (RX .text / RW .data) → patch svc → run     │
│  nx_trampoline.s  guest svc → lkl_syscall → back to guest │
└───────────────────────────────────────────────────────────┘
```

## Build

Everything builds in a Docker container (devkitPro toolchain + libnx):

```bash
docker run -d --name lkl-switch-build -v "$PWD:/project" devkitpro/devkita64:latest sleep infinity
docker exec lkl-switch-build bash -c "apt-get update && apt-get install -y flex bison bc libelf-dev cpio rsync"
docker exec lkl-switch-build bash -c "git clone --depth 1 https://github.com/lkl/linux.git /work/linux"

docker exec lkl-switch-build bash /project/scripts/build-lkl.sh    # cross-compile the kernel
docker exec lkl-switch-build bash /project/scripts/build-nro.sh    # link the NRO + build the guest ELF
```

Build a static-PIE aarch64 busybox to run as the guest:

```bash
docker exec lkl-switch-build bash /project/scripts/build-busybox-pie.sh   # -> busybox
```

Output: `nro/nro.nro` and the `busybox` binary.

## Install & run

1. Copy `nro/nro.nro` → `sd:/switch/lkl-switch.nro`
2. Copy the built `busybox` → `sd:/busybox`
3. Open the **Homebrew Menu** (hold **R** while launching a game, for full memory)
4. Launch **lkl-switch** — it boots Linux and runs `busybox ls /`

## Testing on PC (no Switch needed)

The kernel + all logic is portable C. `scripts/pc-*.sh` build the same code
against the POSIX LKL host and run it under qemu-aarch64 in seconds — the whole
binary-executor chain was developed and debugged this way before touching
hardware.

## Roadmap

- [x] Boot Linux in an NRO
- [x] Syscalls against the live kernel
- [x] ELF binary executor
- [x] **Run real, unmodified busybox on hardware** (`ls`, `uname`, `echo`, ...)
- [ ] More applets + an **interactive shell**. A single applet per launch works
  today; an interactive `sh` needs a keyboard (Switch `swkbd`) wired to stdin and
  a fork story (LKL has no processes — busybox *standalone* mode helps).
  **Contributions very welcome.**
- [ ] Persistent ext4 disk image on the SD (validated on PC; SD block backend WIP)
- [ ] Networking (Horizon `bsd:u` → virtio_net; the TCP stack already inits)

## Credits & license

Built on [LKL](https://github.com/lkl/linux) and
[libnx](https://github.com/switchbrew/libnx)/devkitPro. My code (nx_host,
nx_exec, trampoline, loaders) is MIT; LKL/kernel code keeps its GPL-2.0.
