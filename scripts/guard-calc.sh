#!/bin/bash
# reconstroi o guard com que o exit handler foi mangled no registro
X27=0xfedcba9876143590   # mangled (do gdb)
X2=0xfedcba9876543210    # guard atual no run (do gdb)
CALLFINI=0x400780        # endereco real de call_fini (nm)

echo "x27 (mangled)        = $(printf '0x%x' $X27)"
echo "x2  (guard no run)   = $(printf '0x%x' $X2)"
echo "x27 ^ x2 (demangle)  = $(printf '0x%x' $((X27 ^ X2)))  <- deveria ser call_fini=$CALLFINI"
echo "guard do REGISTRO    = x27 ^ call_fini = $(printf '0x%x' $((X27 ^ CALLFINI)))"
echo ""
echo "Se 'guard do registro' != 'guard no run', o guard MUDOU entre mangle e demangle."
