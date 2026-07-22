#!/bin/bash
# Loop de teste no PC: LKL nativo (POSIX host) + harness.c espelhando o NRO.
set -euo pipefail
LKLSRC=/work/linux-pc

# arvore POSIX separada da cross-aarch64 (nao clobber /work/linux)
# clone limpo do git para nao herdar o .config/Makefile.conf da cross-build
if [ ! -d $LKLSRC ]; then
    git clone --depth 1 file:///work/linux $LKLSRC 2>&1 | tail -1
fi

unset CROSS_COMPILE
make -C $LKLSRC/tools/lkl CROSS_COMPILE= -j8 >/tmp/lkl-pc-build.log 2>&1 || { tail -20 /tmp/lkl-pc-build.log; exit 1; }
LKL=$LKLSRC/tools/lkl

cc /project/pc-test/harness.c -I$LKL/include -o /tmp/harness \
   $LKL/liblkl.a -lpthread -lrt 2>&1 | head -20
echo "=== RUN ==="
rm -f /tmp/lkl-disk.img  # comente esta linha para testar PERSISTENCIA entre runs
timeout 40 /tmp/harness 2>&1 | grep -vE '^\[' | head -50
