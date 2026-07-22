#!/bin/bash
# Fase 2: linka lkl.o (kernel) + camada host libnx + main num NRO
set -euo pipefail
export DEVKITPRO=/opt/devkitpro
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH

LKL=/work/linux/tools/lkl

# kernel sem debug info (324M -> alguns MB de codigo)
if [ ! -f /work/lkl-stripped.o ] || [ $LKL/lib/lkl.o -nt /work/lkl-stripped.o ]; then
    aarch64-none-elf-objcopy --strip-debug $LKL/lib/lkl.o /work/lkl-stripped.o
fi

# .rodata do kernel tem ponteiros que precisam de relocacao em runtime, mas o
# hbloader mapeia o segmento ro como somente-leitura -> Data Abort no boot.
# Renomeia e marca como data (gravavel) pra cair no segmento RW do NRO.
aarch64-none-elf-objcopy --set-section-flags .rodata=alloc,load,contents,data \
    /work/lkl-stripped.o /work/lkl-nro.o
aarch64-none-elf-objcopy --rename-section .rodata=.kernel.rodata /work/lkl-nro.o
ls -lah /work/lkl-nro.o

# fontes genericos portaveis do LKL (nao-especificos de POSIX)
cp $LKL/lib/iomem.c $LKL/lib/jmp_buf.c $LKL/lib/utils.c \
   $LKL/lib/iomem.h $LKL/lib/jmp_buf.h /project/nro/source/

# headers publicos do LKL
mkdir -p /project/nro/include
cp -r $LKL/include/* /project/nro/include/
# autoconf detectou POSIX errado; nosso host nao e POSIX
echo '#define LKL_HOST_CONFIG_AARCH64 y' > /project/nro/include/lkl_autoconf.h

# guest de teste da fase 7 (ELF aarch64 PIE estatico) — vai pro SD junto do NRO
aarch64-none-elf-gcc -nostdlib -static-pie -fPIE -ffreestanding -e _start \
    /project/pc-test/guest.c -o /project/nro/lkl-guest.elf
echo "guest.elf: $(aarch64-none-elf-readelf -h /project/nro/lkl-guest.elf | grep Type)"

export LKL_OBJS=/work/lkl-nro.o
cd /project/nro
make 2>&1 | tail -40
ls -lah *.nro 2>/dev/null || true
