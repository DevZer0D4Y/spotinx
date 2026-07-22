#!/bin/bash
# roda o guest sob qemu com trace e simboliza os ultimos PCs do crash
set -uo pipefail
BIN="${1:-/tmp/showauxv}"
pkill -9 qemu-aarch64 2>/dev/null || true
cd /tmp
timeout 15 qemu-aarch64-static -d cpu,in_asm exec-hw "$BIN" 2>/tmp/q.txt >/dev/null || true
echo "=== ultimos 6 PCs ==="
grep -oE 'PC=[0-9a-f]+' /tmp/q.txt | tail -6
echo "=== simbolizados ==="
for pc in $(grep -oE 'PC=[0-9a-f]+' /tmp/q.txt | tail -6 | cut -d= -f2 | sort -u); do
    sym=$(aarch64-linux-gnu-addr2line -f -C -e exec-hw "0x$pc" 2>/dev/null | tr '\n' ' ')
    echo "0x$pc : $sym"
done
