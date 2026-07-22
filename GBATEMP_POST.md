RASCUNHO — thread do GBAtemp (secao: Switch - Homebrew Development / Homebrews)

=== TÍTULO ===
[Release] lkl-switch — Linux kernel running INSIDE Horizon as an NRO, executing real ELF binaries

=== CORPO ===

Hi all,

I've been working on something I haven't seen done before and wanted to share it.

**TL;DR:** This is the Linux kernel (6.12) compiled as a library (LKL) and linked
*into* a homebrew NRO. It boots inside Horizon OS — no reboot, no L4T, running as
an app — and on top of it there's a from-scratch ELF loader + syscall bridge that
executes an actual aarch64 Linux binary. The guest binary makes real Linux
syscalls (openat/write/close) that hit the live kernel, and I can read back the
file it wrote. All tested on hardware.

**Let me be upfront about what this is NOT**, because I don't want to oversell it:
- No GUI, no browser, no GPU, no desktop. The kernel is built no-MMU.
- It does not replace or compete with L4T Ubuntu. If you want a real Linux
  desktop on your Switch, use L4T — that's the right tool and it's great.
- No interactive shell *yet* (see roadmap).

What it IS: the Linux kernel as a library running inside Horizon, with a working
binary executor. To my knowledge this specific thing (Linux *executing binaries*
from inside Horizon, as homebrew) hasn't been published. I think it's a neat
first and, more usefully, a foundation others can build on.

**How the executor works (the interesting part):**
No CPU emulation, no exception handler. The guest is aarch64, same as the Switch,
so it runs natively. I load the ELF into jitCreate memory, apply relocations, and
rewrite every `svc #0` into a branch to a small trampoline that translates the
Linux syscall ABI (x8=nr, x0–x5=args) into a call into the kernel. Three subtle
bugs took most of the time — PIE relocations, out-of-range branches needing
veneers, and x30 corruption when the svc sits inside a function. Full writeup in
the repo's DEVLOG.

**Screenshots / output:**
[anexar foto da tela mostrando "guest escreveu (27): guest executou via nx_exec!"]

**Source + build instructions:**
[link do GitHub]

Everything builds in a Docker container and there's a PC test harness (qemu-aarch64)
so you can hack on it without a Switch.

**Roadmap / help wanted:**
The engine is done; the frontier is an interactive busybox shell — needs
/dev/console wired to keyboard/screen, a lot more syscalls, and a fork story
(LKL has no processes; busybox standalone mode should help). If anyone with LKL
or no-MMU userland experience wants to jump in, PRs and ideas very welcome.

Happy to answer questions. Thanks to the LKL and libnx/devkitPro folks whose work
this stands on.

=== NOTAS PRA VOCE (nao postar) ===
- Anexa 1-2 fotos reais da tela (a da fase 7 e a do "Linux vivo" da fase 3).
- Poe o link do GitHub depois de criar o repo.
- Tom: humilde + preciso. A cena respeita quem admite limites; quem promete
  demais leva "isso e fake" nos comentarios. Voce ja tem o print que prova.
- Se perguntarem "roda [jogo/programa X]": nao, so binarios estaticos simples
  por enquanto; sem fork/GUI. Aponta pro roadmap.
