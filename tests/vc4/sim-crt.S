/*
 * Start file for test programs built and linked by vc4-tcc alone, for the
 * resim simulator (SPDX-License-Identifier: LGPL-2.1-or-later). Linked
 * first at address 0; resim starts there and stops at "sleep".
 */
	.text
	.globl	_start
_start:
	mov	sp, 0x07ff0000
	bl	main
	sleep
1:	b	1b
