/*
 * nx_trampoline.s - trampolim de syscall do guest (fase 7). Validado no PC.
 *
 * Entrada (vinda do veneer): x17 = endereco de retorno no guest (apos o svc),
 * x8 = numero da syscall, x0..x5 = args. NAO usa x30 (o svc foi trocado por
 * `b veneer`, branch sem link, entao o x30 do guest fica intacto para o `ret`
 * do helper de syscall). Empacota os regs, chama nx_do_syscall, poe o retorno
 * em x0 e volta pro guest via `br x17`.
 */
	.text
	.global nx_svc_trampoline
	.type nx_svc_trampoline, %function
	.balign 4
nx_svc_trampoline:
	sub	sp, sp, #80
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	str	x8, [sp, #48]
	stp	x17, x30, [sp, #56]	/* x17=retorno guest, x30=preserva o nosso */

	mov	x0, sp			/* &regs */
	bl	nx_do_syscall		/* retorno em x0 */

	ldp	x17, x30, [sp, #56]	/* restaura */
	add	sp, sp, #80
	br	x17			/* volta pro guest apos o svc; x30 intacto */
	.size nx_svc_trampoline, .-nx_svc_trampoline
