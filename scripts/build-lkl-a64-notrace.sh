#!/bin/bash
# Recompila o liblkl aarch64-linux com TRACING desabilitado, pra testar a
# hipotese (4a pesquisa) de que o crash em filter_assign_type/strstr e o
# subsistema de trace events (ou corrupcao visivel atraves dele).
set -uo pipefail
LKLA=/work/linux-a64
CROSS=aarch64-linux-gnu-

cd $LKLA
# desliga tracing/ftrace na config do LKL
CFG=arch/lkl/configs/defconfig
for opt in CONFIG_FTRACE CONFIG_TRACING CONFIG_EVENT_TRACING CONFIG_TRACEPOINTS \
           CONFIG_FTRACE_SYSCALLS CONFIG_ENABLE_DEFAULT_TRACERS; do
    echo "# $opt is not set" >> $CFG
done

make -C tools/lkl clean >/dev/null 2>&1 || true
make -C tools/lkl CROSS_COMPILE=$CROSS -j8 >/tmp/lkl-notrace.log 2>&1 \
    && echo "LKL sem trace: OK" || { tail -20 /tmp/lkl-notrace.log; exit 1; }
