#!/bin/bash
set -euo pipefail
LKL=/work/linux-pc/tools/lkl
cc /project/pc-test/exec-poc.c -I"$LKL/include" -o /tmp/exec-poc \
   "$LKL/liblkl.a" -lpthread -lrt 2>&1 | grep -vE 'GNU-stack|deprecated' || true
echo "=== RUN ==="
timeout 40 /tmp/exec-poc 2>&1 | grep -vE '^\[' | head -20
