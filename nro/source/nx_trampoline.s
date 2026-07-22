/*
 * nx_trampoline.s - trampolim de syscall do guest (fase 8). Validado no PC como
 * pc_trampoline em pc-test/exec-hw.c. Preservacao COMPLETA de estado que um `svc`
 * inline preserva mas uma chamada C pode destruir: x0-x18, x30, NZCV/FPCR/FPSR,
 * q0-q31 (SIMD), e o TPIDR_EL0 do guest. Troca pro TP e stack do host antes do C.
 *
 * Entrada (do veneer): x16 = endereco do veneer de SAIDA; x8=nr, x0..x5=args.
 * x16/x17 do guest ja foram salvos pelo veneer de entrada no stack do guest.
 * Frame (736B): x0-x15 em 0..120; x18,x30 em 128; x16exit,gtp em 144; nzcv,fpcr
 * em 160; fpsr em 176; x19 em 184; x20,x21 em 192; result em 208; q0-q31 em 224.
 */
	.text
	.global nx_svc_trampoline
	.type nx_svc_trampoline, %function
	.balign 4
nx_svc_trampoline:
	sub	sp, sp, #736
	stp	x0, x1,   [sp, #0]
	stp	x2, x3,   [sp, #16]
	stp	x4, x5,   [sp, #32]
	stp	x6, x7,   [sp, #48]
	stp	x8, x9,   [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x18, x30, [sp, #128]
	mrs	x9, tpidr_el0
	stp	x16, x9,  [sp, #144]
	mrs	x9, nzcv
	mrs	x10, fpcr
	stp	x9, x10,  [sp, #160]
	mrs	x9, fpsr
	str	x9,       [sp, #176]
	str	x19,      [sp, #184]
	stp	x20, x21, [sp, #192]
	stp	q0, q1,   [sp, #224]
	stp	q2, q3,   [sp, #256]
	stp	q4, q5,   [sp, #288]
	stp	q6, q7,   [sp, #320]
	stp	q8, q9,   [sp, #352]
	stp	q10, q11, [sp, #384]
	stp	q12, q13, [sp, #416]
	stp	q14, q15, [sp, #448]
	stp	q16, q17, [sp, #480]
	stp	q18, q19, [sp, #512]
	stp	q20, q21, [sp, #544]
	stp	q22, q23, [sp, #576]
	stp	q24, q25, [sp, #608]
	stp	q26, q27, [sp, #640]
	stp	q28, q29, [sp, #672]
	stp	q30, q31, [sp, #704]
	mov	x19, sp			/* guest frame (callee-saved) */
	adrp	x10, g_host_tp
	ldr	x10, [x10, #:lo12:g_host_tp]
	msr	tpidr_el0, x10
	adrp	x10, g_host_stack
	ldr	x10, [x10, #:lo12:g_host_stack]
	mov	sp, x10
	mov	x0, x19
	bl	nx_do_syscall
	str	x0, [x19, #208]		/* salva resultado */
	mov	sp, x19
	ldp	q0, q1,   [sp, #224]
	ldp	q2, q3,   [sp, #256]
	ldp	q4, q5,   [sp, #288]
	ldp	q6, q7,   [sp, #320]
	ldp	q8, q9,   [sp, #352]
	ldp	q10, q11, [sp, #384]
	ldp	q12, q13, [sp, #416]
	ldp	q14, q15, [sp, #448]
	ldp	q16, q17, [sp, #480]
	ldp	q18, q19, [sp, #512]
	ldp	q20, q21, [sp, #544]
	ldp	q22, q23, [sp, #576]
	ldp	q24, q25, [sp, #608]
	ldp	q26, q27, [sp, #640]
	ldp	q28, q29, [sp, #672]
	ldp	q30, q31, [sp, #704]
	ldr	x9, [sp, #152]		/* guest TP */
	msr	tpidr_el0, x9
	ldp	x9, x10, [sp, #160]
	msr	nzcv, x9
	msr	fpcr, x10
	ldr	x9, [sp, #176]
	msr	fpsr, x9
	ldr	x16, [sp, #144]		/* endereco do veneer de saida */
	ldp	x18, x30, [sp, #128]
	ldp	x14, x15, [sp, #112]
	ldp	x12, x13, [sp, #96]
	ldp	x10, x11, [sp, #80]
	ldp	x8, x9,   [sp, #64]
	ldp	x6, x7,   [sp, #48]
	ldp	x4, x5,   [sp, #32]
	ldp	x2, x3,   [sp, #16]
	ldp	x0, x1,   [sp, #0]
	ldr	x19,      [sp, #184]
	ldp	x20, x21, [sp, #192]
	ldr	x0,       [sp, #208]	/* ABI: resultado em x0 */
	add	sp, sp, #736
	br	x16			/* veneer de saida: restaura x16/x17, b site+4 */
	.size nx_svc_trampoline, .-nx_svc_trampoline
