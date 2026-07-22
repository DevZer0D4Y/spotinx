#!/bin/bash
# busybox aarch64 STATIC-PIE para o Switch (jitCreate da base arbitraria; PIE roda
# em qualquer base). O conflito "-r and -pie" e porque built-in.o usa -r; entao
# compilamos com -fPIE e passamos -static-pie SO no link final do executavel via
# a var CFLAGS_busybox (aplicada so no alvo final).
set -uo pipefail
BB=/work/busybox-1.36.1
CROSS=aarch64-linux-gnu-

cd $BB
make ARCH=arm64 CROSS_COMPILE=$CROSS defconfig >/dev/null 2>&1
sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
sed -i 's|CONFIG_EXTRA_CFLAGS=.*|CONFIG_EXTRA_CFLAGS="-fPIE"|' .config
yes '' | make ARCH=arm64 CROSS_COMPILE=$CROSS oldconfig >/dev/null 2>&1 || true

# CFLAGS_busybox e adicionado SO no comando de link do executavel final
make ARCH=arm64 CROSS_COMPILE=$CROSS -j8 \
     CFLAGS_busybox="-static-pie" busybox >/tmp/bb-pie.log 2>&1 \
     || { echo "FALHOU:"; tail -18 /tmp/bb-pie.log; exit 1; }

echo "=== OK ==="
${CROSS}readelf -h busybox | grep -E 'Type:|Entry'
cp busybox /work/busybox-pie
ls -lah /work/busybox-pie
