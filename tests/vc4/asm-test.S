/*
 * Assembler test for vc4-tcc (SPDX-License-Identifier: LGPL-2.1-or-later):
 * asm-test.sh assembles this file with vc4-elf-as and with vc4-tcc; the
 * linked bytes must be the same. Only forms where both pick the same
 * encoding are here (gas resolves forward branches itself and has a 16-bit
 * sp-relative load/store; vc4-tcc does neither). gas reads a register as
 * a symbol in "add r20, r21" (high registers, two operands): not used.
 */
#define TWICE(x) ((x) * 2)

	.text
	.globl	start
start:
	nop
	rts
	rti
	di
	ei
	sleep
	bkpt
	version r0
	version r15
	mov	r0, r1
	mov	r20, r1
	mov	r1, r20
	mov	r0, 31
	mov	r0, #7
	mov	r0, 32
	mov	r0, -1
	mov	r3, 0x7fff
	mov	sp, 0x8001b000
	mov	r1, sp
	mov	r0, TWICE(10)
	mov	r5, start
	mov	r5, data_label+4
	add	r0, r1
	sub	r2, r3
	mul	r0, r1
	and	r0, r1
	or	r0, r1
	eor	r0, r1
	bic	r0, r1
	shl	r0, r1
	lsr	r0, r1
	asr	r0, r1
	ror	r0, r1
	cmp	r0, r1
	cmn	r0, r1
	neg	r0, r1
	not	r0, r1
	min	r0, r1
	max	r0, r1
	abs	r0, r1
	btest	r0, r1
	msb	r0, r1
	brev	r0, r1
	rsub	r0, r1
	add	r0, 5
	sub	r0, 31
	cmp	r0, 7
	shl	r1, 24
	asr	r1, 24
	add	r0, 1000
	add	r0, r1, r2
	add	r0, r1, 5
	add	r16, r17, r18
	sub	r20, r21, -3
	neg	r20, r21
	not	r20, r21
	cmp	r20, r21
	ld	r0, (r1)
	st	r2, (r3)
	ldh	r0, (r1)
	ldb	r2, (r3)
	sth	r0, (r1)
	stb	r0, (r1)
	ldsh	r0, (r1)
	ld	r0, (r1+8)
	st	r0, (r1+60)
	ld	r0, (r1+64)
	ld	r20, (r1+4)
	ldb	r0, (r1+3)
	ld	r0, (r1+0x10000)
	b	r0
	bl	r1
	bl	far_away
	bl	start
1:	b	1b
	beq	1b
	bne	start
	bgt	1b
	b	start
	.irp	n, 0, 1, 2
irp_\n:
	mov	r0, \n
	.endr
	bl	irp_2

	.data
data_label:
	.long	0x11223344, start, irp_1
	.short	5
	.byte	1, 2
	.align	2
	.long	data_label
	.balign	8
	.space	3
	.byte	9
	.incbin	INCFILE
	.balign	8
