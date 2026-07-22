#!/bin/bash
# Fase 1: cross-compile do LKL (kernel como lib) com devkitA64
set -uo pipefail
export DEVKITPRO=/opt/devkitpro
export PATH=/opt/devkitpro/devkitA64/bin:$PATH

cd /work/linux
make -C tools/lkl clean-conf >/dev/null 2>&1 || true
make -C tools/lkl CROSS_COMPILE=aarch64-none-elf- /work/linux/tools/lkl/lib/lkl.o 2>&1 | tail -40
