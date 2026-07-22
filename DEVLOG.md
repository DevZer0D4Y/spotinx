# lkl-switch

Linux Kernel Library (LKL) rodando dentro de um NRO no Horizon OS (Nintendo Switch).

## Arquitetura

- **LKL** (github.com/lkl/linux): kernel Linux compilado como biblioteca estática.
- **Camada host Horizon** (`host/`): implementa as ~15 lkl_host_operations usando libnx
  (threads, semáforos, mutex, alocação, timers, print no console).
- **NRO** (`nro/`): app homebrew que linka tudo, inicializa o console e chama
  `lkl_start_kernel()`. Meta da fase 1: dmesg na tela do Switch.

## Ambiente de build

Tudo compila dentro do container Docker `lkl-switch-build` (base: devkitpro/devkita64,
que traz o cross-compiler aarch64 + libnx). O fonte do kernel mora DENTRO do container
(filesystem Linux, case-sensitive). Este diretório do Windows é montado em /project.

```powershell
# recriar o container do zero (se necessário)
docker run -d --name lkl-switch-build -v C:\Projetos\ProjetosPessoais\NintendoSwitch\lkl-switch:/project devkitpro/devkita64:latest sleep infinity
docker exec lkl-switch-build bash -c "apt-get update && apt-get install -y flex bison bc libelf-dev cpio rsync"
docker exec lkl-switch-build bash -c "git clone --depth 1 https://github.com/lkl/linux.git /work/linux"

# build do NRO (fase 0: hello world para validar toolchain)
docker exec lkl-switch-build bash -c "cd /project/nro && make"
```

Saída: `nro/lkl-switch.nro` → copiar para `sd:/switch/` no cartão SD.

## Fases

- [x] Fase 0: NRO hello-world compilando e rodando no Switch (testado no hardware)
- [x] Fase 1: LKL compilado com devkitA64 — `scripts/build-lkl.sh` gera
      /work/linux/tools/lkl/lib/lkl.o (ELF aarch64 relocatable, kernel completo)
- [x] Fase 2: BOOT! Linux 6.12.0 bootou no Horizon OS em 2026-07-21 (build v5).
      dmesg completo ate "Run /init as init process". Correcoes que importaram:
      .rodata do kernel -> segmento RW (relocacoes), tabela chama-se lkl_host_ops.
      Instrumentacao de debug (oplog/stage) em nx_host.c — remover p/ boot ~1s.
- [x] Fase 3: syscalls (uname/tmpfs/arquivo) — FECHADA NO HARDWARE, LIMPA (v14, 2026-07-22):
      estagio "F" com oplog DESLIGADO, sem muleta de debug. uname/tmpfs/read-write OK.
      CAUSA RAIZ (levou v10-v13 pra isolar): o timer thread (nx_timer) e o TICK do
      escalonador do Linux — cada disparo destrava a proxima troca de contexto. Ele
      rodava na MESMA prioridade (0x2C) das threads do kernel; quando elas monopolizavam
      o core, o timer nunca era escalonado e o tick nao chegava → boot congelava no fim.
      O fflush por-op do oplog mascarava (forcava as threads a bloquear no I/O do SD).
      CORRECAO: timer thread com prioridade mais alta (0x20) — garante que o tick sempre
      chega. NAO era race, nem starvation de core, nem semaforo — era prioridade do timer.
  LOOP DE TESTE NO PC: scripts/pc-test.sh compila LKL POSIX (/work/linux-pc) e roda
  harness.c espelhando o NRO. Desenvolver fases 4-7 aqui antes do hardware.
  (Ryujinx 1.3.3 baixado em tools/ryubing mas nao carrega o NRO por CLI de forma util.)
- [x] Fase 4: disco ext4 — VALIDADA no PC (harness.c: lkl_disk_add + lkl_mount_dev
      montam ext4 real, arquivo persiste). No Switch: nx_host fornece lkl_dev_blk_ops
      lendo/escrevendo imagem no SD (fread/fwrite) — codigo de hardware pendente.
- [~] Fase 5: terminal (swkbd + shell host sobre syscalls) — camada de UI, miolo
      (VFS) provado. So no hardware.
- [~] Fase 6: rede (bsd:u -> virtio_net) — pilha TCP sobe no dmesg; backend host
      pendente. So no hardware.
- [x] Fase 7 (objetivo final): EXECUTOR DE BINARIOS — as duas metades VALIDADAS no PC:
      * PONTE DE ABI (pc-test/exec-poc.c): svc guest (x8=nr,x0-x5) -> svc_trampoline
        -> lkl_syscall -> efeito no VFS. Confirmado: guest openat/write/close roda.
      * LOADER ELF (pc-test/elf-loader.c): parse PT_LOAD, mapeia segmentos, BSS
        zerado, calcula entry. Confirmado com ELF aarch64 estatico real.
      CADEIA COMPLETA VALIDADA no PC via qemu-aarch64 (2026-07-22, scripts/pc-exec-hw.sh
      + pc-test/exec-hw.c + guest.c): ELF aarch64 PIE real carregado, relocado, svc
      patchados, saltou no entry, 3 syscalls (openat/write/close) pelo trampolim ->
      lkl_syscall, host leu o arquivo que o guest escreveu. 3 bugs reais resolvidos
      (so aparecem com binario de verdade): (1) PIE+R_AARCH64_RELATIVE senao endereco
      absoluto quebra fora da base; (2) bl nao alcanca trampolim a >128MB -> veneers;
      (3) bl corrompe x30 do guest (svc estava dentro de sys(), loop infinito) ->
      veneer-por-svc com `b` (sem link) preserva x30, trampolim volta via x17.
      SEM handler de excecao: patch svc->b (o jitCreate no Switch da a memoria RX).
      PENDENTE HARDWARE: portar essas 3 correcoes pro nx_exec.c do Switch e testar.
      Modelo no-MMU. Alvo seguinte: busybox sh.

Scripts de teste no PC (segundos, sem Switch): scripts/pc-test.sh (fases 3+4),
scripts/pc-exec.sh (ponte ABI), scripts/pc-loader.sh (loader ELF).
