# lkl-switch

**Linux running as a homebrew app inside Horizon OS — and executing real Linux ELF binaries — on a hacked Nintendo Switch.**

Not L4T. Not a reboot. This is the Linux kernel (6.12) linked *into* an NRO
homebrew, booting and running alongside Horizon OS, plus a from-scratch ELF
loader + syscall bridge that runs an actual aarch64 Linux binary on top of it.

As far as I can tell, nobody has published Linux *executing binaries* from
inside Horizon before. L4T replaces the system at boot; this runs as an app.

> ⚠️ **What this is not:** no GUI, no browser, no GPU, no desktop. The kernel
> has no MMU (`CONFIG_MMU` off) and cannot run graphical software. If you want a
> Linux desktop on your Switch, use [L4T Ubuntu](https://wiki.switchroot.org/).
> This project is a different thing: the Linux kernel as a library inside a
> homebrew, running real binaries — a technical first, and a foundation to build on.

## What works (tested on hardware)

- **Boots Linux 6.12** inside an NRO — full dmesg, on-screen.
- **Runs syscalls** against the live kernel: `uname`, mount `tmpfs`, create/read files.
- **Executes a real aarch64 ELF binary**: loads it, applies relocations, patches
  its `svc` instructions, jumps to its entry, and every `svc` the binary makes
  routes through a custom trampoline into `lkl_syscall`. Proven by having the
  guest binary write a file *through our kernel* and reading it back.

```
=== Fase 7: executor de binarios ===
nx_exec: 1 svc->veneer, entry=0x264
nx_exec: saltando no guest...
nx_exec: guest retornou
*** guest escreveu (27): guest executou via nx_exec! ***
```

## How it works

```
┌─ NRO (homebrew) ─────────────────────────────────────────┐
│  main.c        boots LKL on a dedicated thread            │
│  nx_host.c     lkl_host_operations via libnx              │
│                (threads, semaphores, timers, console)     │
│  ┌─ LKL (Linux 6.12, ARCH=lkl, no-MMU) ────────────────┐  │
│  │  full kernel: VFS, tmpfs, ext4, TCP/IP stack        │  │
│  └─────────────────────────────────────────────────────┘  │
│  nx_exec.c     ELF loader → jitCreate (RX) → patch svc    │
│  nx_trampoline.s  guest svc → lkl_syscall → back to guest │
└───────────────────────────────────────────────────────────┘
```

The binary executor has no CPU emulation and no exception handler. The guest
binary runs natively (it's aarch64, same as the Switch); we just rewrite its
`svc #0` instructions to branch into a trampoline that translates the Linux
syscall ABI (x8=nr, x0–x5=args) into a `lkl_syscall()` call. See
[DEVLOG.md](DEVLOG.md) for the three subtle bugs this took to get right
(PIE relocations, out-of-range branches, and `x30` corruption).

## Build

Everything builds in a Docker container (devkitPro toolchain + libnx):

```bash
docker run -d --name lkl-switch-build -v "$PWD:/project" devkitpro/devkita64:latest sleep infinity
docker exec lkl-switch-build bash -c "apt-get update && apt-get install -y flex bison bc libelf-dev cpio rsync"
docker exec lkl-switch-build bash -c "git clone --depth 1 https://github.com/lkl/linux.git /work/linux"

docker exec lkl-switch-build bash /project/scripts/build-lkl.sh    # cross-compile the kernel
docker exec lkl-switch-build bash /project/scripts/build-nro.sh    # link the NRO + build the guest ELF
```

Output: `nro/nro.nro` and `nro/lkl-guest.elf`.

## Install & run

1. Copy `nro/nro.nro` → `sd:/switch/lkl-switch.nro`
2. Copy `nro/lkl-guest.elf` → `sd:/lkl-guest.elf`
3. Open the **Homebrew Menu** (hold **R** while launching a game, for full memory)
4. Launch **lkl-switch**

## Testing on PC (no Switch needed)

The kernel + all logic is portable C. `scripts/pc-*.sh` build the same code
against the POSIX LKL host and run it under qemu-aarch64 in seconds — the whole
binary-executor chain was developed and debugged this way before touching
hardware.

## Roadmap

- [x] Boot Linux in an NRO
- [x] Syscalls against the live kernel
- [x] ext4 disk image (validated on PC; SD block backend WIP on hardware)
- [x] **ELF binary executor** ← the goal
- [ ] **Interactive busybox shell** — the next big milestone. Needs
  `/dev/console` wired to the Switch keyboard/screen, dozens more syscalls, and
  a fork story (LKL has no processes — busybox *standalone* mode helps).
  **Contributions very welcome — the engine is done, the shell is the frontier.**
- [ ] Networking (Horizon `bsd:u` → virtio_net; the TCP stack already inits)

## Credits & license

Built on [LKL](https://github.com/lkl/linux) and
[libnx](https://github.com/switchbrew/libnx)/devkitPro. My code (nx_host,
nx_exec, trampoline, loaders) is MIT; LKL/kernel code keeps its GPL-2.0.
