#!/bin/bash
set -euo pipefail
export PATH=/opt/devkitpro/devkitA64/bin:$PATH

# 1) um guest aarch64 minimo, estatico, freestanding (so pra ter secoes reais)
cat > /tmp/guest.c <<'EOF'
static long sys(long nr, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8),"r"(x1),"r"(x2) : "memory");
    return x0;
}
static const char msg[] = "ola do guest ELF\n";
void _start(void) {
    sys(64, 1, (long)msg, sizeof(msg)-1); /* write(1,msg,len) */
    sys(93, 0, 0, 0);                     /* exit(0) */
}
EOF
aarch64-none-elf-gcc -nostdlib -static -ffreestanding -e _start \
    /tmp/guest.c -o /tmp/guest.elf
echo "guest.elf:"; aarch64-none-elf-readelf -h /tmp/guest.elf | grep -E 'Machine|Type|Entry'

# 2) o loader (compila nativo, so faz parsing/mapeamento)
cc /project/pc-test/elf-loader.c -o /tmp/elf-loader
echo "=== RUN loader ==="
/tmp/elf-loader /tmp/guest.elf
