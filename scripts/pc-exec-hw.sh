#!/bin/bash
# Valida a cadeia completa da fase 7 no PC, em aarch64 via qemu-user.
# Compila liblkl aarch64-linux (separado do Switch/freestanding e do x86),
# compila o guest e o exec-hw, roda tudo sob qemu-aarch64-static.
set -euo pipefail

LKLA=/work/linux-a64
CROSS=aarch64-linux-gnu-

# liblkl aarch64-linux (POSIX host, glibc) — clone limpo, sem herdar .config
if [ ! -f $LKLA/tools/lkl/liblkl.a ]; then
    [ -d $LKLA ] || git clone --depth 1 file:///work/linux $LKLA 2>&1 | tail -1
    make -C $LKLA/tools/lkl CROSS_COMPILE=$CROSS -j8 >/tmp/lkl-a64.log 2>&1 \
        || { tail -25 /tmp/lkl-a64.log; exit 1; }
fi
LKL=$LKLA/tools/lkl

# guest de teste (freestanding, PIE — enderecos relativos ao PC, roda em qualquer
# base de carga; ELF EXEC nao-PIE usa enderecos absolutos e quebra fora de 0x400000)
${CROSS}gcc -nostdlib -static-pie -fPIE -ffreestanding -e _start \
    /project/pc-test/guest.c -o /tmp/guest.elf

# executor
${CROSS}gcc /project/pc-test/exec-hw.c -I$LKL/include -o /tmp/exec-hw \
    $LKL/liblkl.a -lpthread -lrt -static 2>&1 | grep -vE 'GNU-stack|deprecated' || true

echo "=== RUN (qemu-aarch64) ==="
qemu-aarch64-static /tmp/exec-hw /tmp/guest.elf 2>&1 | grep -vE '^\[' | head -25
