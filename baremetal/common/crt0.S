/*
 * Startup code and exception entry for the bare-metal tests.
 *
 * Boot CPU: entered at _start by QEMU (SR=0x2700, MMU off, VBR=0).
 * Secondary CPUs: entered at _secondary_start with %d0 = CPU index
 * (set up through the virt-smp START registers, see doc/virt-smp-spec.md).
 */

#define STACK_SIZE	16384
#define MAX_CPUS	16

	.section .text.start,"ax"
	.globl	_start
_start:
	movew	#0x2700,%sr
	lea	_stacks+STACK_SIZE,%sp
	/* Clear BSS.  The stacks live in their own section and are not
	 * cleared.  The bootinfo list follows _end (see link.ld). */
	lea	_bss_start,%a0
	lea	_bss_end,%a1
1:	cmpal	%a1,%a0
	jcc	2f
	clrl	%a0@+
	jra	1b
2:
	lea	_vectors,%a0
	movec	%a0,%vbr
	clrl	%sp@-			/* cpu 0 */
	jsr	cpu_main
	jra	_halt

	.text
	.globl	_secondary_start
_secondary_start:
	movew	#0x2700,%sr
	/* sp = _stacks + (cpu + 1) * STACK_SIZE */
	movel	%d0,%d1
	addql	#1,%d1
	mulul	#STACK_SIZE,%d1
	lea	_stacks,%sp
	addal	%d1,%sp
	lea	_vectors,%a0
	movec	%a0,%vbr
	movel	%d0,%sp@-
	jsr	cpu_main
	jra	_halt

	.globl	_halt
_halt:
	stop	#0x2700
	jra	_halt

/*
 * Generic exception entry.  All 256 vectors point here; the vector
 * number is recovered from the format/vector word of the frame.
 * C handler: void exc_dispatch(struct exc_frame *f, uint32_t *regs)
 * where regs[] = d0-d7/a0-a6 as saved.
 */
	.globl	_exc_entry
_exc_entry:
	moveml	%d0-%d7/%a0-%a6,%sp@-
	movel	%sp,%a0
	pea	%a0@
	pea	%a0@(60)
	jsr	exc_dispatch
	addql	#8,%sp
	moveml	%sp@+,%d0-%d7/%a0-%a6
	rte

	.section .vectors,"aw"
	.balign	1024
	.globl	_vectors
_vectors:
	.rept	256
	.long	_exc_entry
	.endr

	/* Stacks are not cleared (they are live while BSS is cleared). */
	.section .stacks,"aw",@nobits
	.balign	16
	.globl	_stacks
_stacks:
	.space	STACK_SIZE * MAX_CPUS
