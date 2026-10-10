# Chromium NX

**A Chromium web browser running as a homebrew app inside Horizon OS, on a
hacked Nintendo Switch.**

Chromium NX is an ordinary NRO started from
the Homebrew Menu. Inside it, the Linux kernel runs as a library (LKL), and on
top of that kernel runs the stock arm64 Chromium binary from Debian,
unmodified, drawing with the console's GPU.

This is experimental software.

## What it does

- Runs Debian's `chromium-shell` 154 (the Chromium engine with a minimal
  window), as shipped, without recompiling it.
- Renders with the GPU: ANGLE inside Chromium talks to the console's own
  EGL/GLES driver
- Runs JavaScript
- Touch screen, on-screen keyboard, and the pad:

  | Input | Action |
  |---|---|
  | Touch | Tap, drag, scroll; touching a text field opens the keyboard |
  | D-pad | Moves the focus from link to link |
  | A | Follows the focused link, or opens the keyboard on a text field |
  | B / X | Back / reload |
  | Y | Opens the keyboard on the address bar |
  | L / R | Backspace / Enter |
  | Sticks | Scroll |
  | + | Quit |

- HTTPS networking through the console's Wi-Fi.
- Sound, through the console's audio output.

## Status

Confirmed on a real console:

- Google, Wikipedia and YouTube load and are usable; YouTube videos play.
- GPU rendering and the JIT.
- Touch, scrolling, text entry with the system keyboard, d-pad focus.

Known problems and limits:

- **Nothing is kept between sessions**: no cookies, no cache, no history.
- **The interface is `chromium-shell`'s**: four text buttons and an address
  bar. No tabs, bookmarks, settings page or extensions.

## Install

1. Build (below), or get the three files from someone who did.
2. Back up `atmosphere/hbl.nsp` on the SD card, then replace it with
   `gx/dist-hbl/atmosphere/hbl.nsp`.
3. Copy the folder `gx/dist-nx/switch/chromium-nx/` to `/switch/` on the SD
   card. It holds `chromium-nx.nro` and `chromium.ext4` (Chromium and the
   Linux files around it).
4. Start **Chromium NX** from the Homebrew Menu. It opens google.com.

The log of the last run is `/switch/chromium-nx/chromium-nx.log`.

## How it works

```
┌─ chromium-nx.nro ──────────────────────────────────────────────┐
│  host_nx.c   the Horizon side: memory, threads, screen, pad,    │
│              sound, the SD card as a disk, the network          │
│  ┌─ LKL: Linux as a library (no MMU) ────────────────────────┐  │
│  │  files (ext4 image, tmpfs), sockets, pipes, epoll, ...    │  │
│  └───────────────────────────────────────────────────────────┘  │
│  gx: runs Linux programs on that kernel                         │
│    gelf.c   loads dynamically linked ELF programs               │
│    gmem.c   the guest's address space (mmap, mprotect, ...)     │
│    gsys.c   system calls: most go to LKL, memory and threads    │
│             are done here                                       │
│    tramp.S  every `svc` of the guest is patched into a jump     │
│             here                                                │
│    wl.c     a Wayland compositor: Chromium's window and inputs  │
│    cdp.c    Chromium's debugging pipe, for what a window does   │
│             not say (is the focus on a text field?)             │
│    nat.c    a user-mode NAT between LKL's network and Horizon's │
│             sockets                                             │
│  ┌─ guest ───────────────────────────────────────────────────┐  │
│  │  chromium-shell (Debian arm64), glibc and its libraries   │  │
│  │  libEGL / libGLESv2 / libasound: small replacements that  │  │
│  │  call the host's drivers                                  │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

The guest runs natively: it is arm64 code on an arm64 processor, with no
emulation. What the kernel of a real Linux would do for it is split between
LKL (files, network) and gx (memory, threads, time).

A few things Horizon does differently had to be worked around:

- **No memory that is writable and executable.** A page of V8's code area is
  one or the other; the fault of a write to an executable page, or of a jump
  to a writable one, flips it.
- **Shared memory is scarce** (80 objects for the whole console) and must be
  mapped whole, so shared files are pooled into a few large pieces.
- **The kernel keeps a record per run of pages mapped alike**, and stops
  serving a process past about ten thousand of them, so guest memory is
  committed in large units.
- **Reading the virtual counter traps**; it is emulated in the exception
  handler.

## Build

Everything builds in two Docker containers that share a volume: `lkl-gx`
(Debian 12 with arm64 packages, for the PC test bench and Chromium's root
image) and `lkl-gx-nx` (devkitPro's devkitA64, for the console). Each script
says at its top how its container is made. `gx/docker/Dockerfile` describes the
first one but has not been rebuilt from scratch recently.

```bash
docker exec lkl-gx    bash /project/gx/build-lkl.sh        # LKL for the PC bench
docker exec lkl-gx    bash /project/gx/build-pc.sh         # gx for PC, guest libraries, tests
docker exec lkl-gx    bash /project/gx/build-chromium.sh   # Chromium's root images
docker exec lkl-gx-nx bash /project/gx/build-nx.sh         # the NRO -> gx/dist-nx
docker exec lkl-gx-nx bash /project/gx/build-hbl.sh        # the loader -> gx/dist-hbl
```

`DEV=1 build-nx.sh` adds a `programs.txt` next to the NRO: the app then shows
a menu of what to run (test programs, Chromium without the GPU or the JIT)
instead of starting the browser.

## Testing on a PC

The same code builds for an arm64 Linux host and runs under `qemu-aarch64`,
with the same restrictions the console imposes (no writable and executable
memory, pooled shared memory). `build-pc.sh` runs the small tests;
`build-chromium.sh wayland [url]` runs Chromium with a scripted user and
writes the pictures it draws to files. Most of the work was done this way
before touching hardware.

## Layout

- `gx/` — everything described above.
- `nro/`, `scripts/`, `pc-test/`, `DEVLOG.md` — the original lkl-switch
  project this one grew from: its NRO that boots LKL and runs busybox. gx
  reuses its `nro/source/nx_host.c` (LKL's host operations on libnx).

## Credits

- **mnnobre** ([mnnobre/lkl-switch](https://github.com/mnnobre/lkl-switch))
  — the original project: Linux as a library inside an NRO, and the first
  executor of Linux binaries on it.
- [LKL](https://github.com/lkl/linux), [libnx](https://github.com/switchbrew/libnx)
  and devkitPro, [nx-hbloader](https://github.com/switchbrew/nx-hbloader),
  Mesa, and of course [Chromium](https://www.chromium.org/).

The Linux kernel (LKL) is GPL-2.0. The original lkl-switch code is MIT
according to its author. Chromium and its logo are the Chromium Authors'.
Chromium NX is not affiliated with Google or Nintendo.
