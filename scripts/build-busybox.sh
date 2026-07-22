#!/bin/bash
# Cross-compila busybox aarch64 estatico-PIE para testar no executor da fase 8.
# static-PIE: estatico (sem libc dinamica) + PIE (nosso loader aplica relocs e
# carrega em base arbitraria via jit/mmap).
set -euo pipefail
BB=/work/busybox-1.36.1
CROSS=aarch64-linux-gnu-

cd $BB
make ARCH=arm64 CROSS_COMPILE=$CROSS defconfig >/dev/null 2>&1

# AGORA edita o .config (depois do defconfig, senao ele sobrescreve)
sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
sed -i 's/# CONFIG_PIE is not set/CONFIG_PIE=y/' .config

# reconcilia deps do config sem prompts
yes '' | make ARCH=arm64 CROSS_COMPILE=$CROSS oldconfig >/dev/null 2>&1
echo "config: $(grep -E 'CONFIG_STATIC=|CONFIG_PIE=' .config | tr '\n' ' ')"

make ARCH=arm64 CROSS_COMPILE=$CROSS -j8 busybox >/tmp/bb-build.log 2>&1 || {
    echo "BUILD FALHOU:"; tail -25 /tmp/bb-build.log; exit 1; }

echo "=== OK ==="
aarch64-linux-gnu-readelf -h $BB/busybox | grep Type
ls -lah $BB/busybox
cp $BB/busybox /work/busybox-a64
