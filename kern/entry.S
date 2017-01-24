//copyright@Yiru Chen	
#include <inc/memlayout.h>

// Ref. http://wiki.osdev.org/ARM_RaspberryPi_Tutorial_C

// To keep this in the first portion of the binary.
.section ".text.boot"
 
// Make _start global.
.globl _start
 
// Entry point for the kernel.
// r15 -> should begin execution at 0x8000.
// r0 -> 0x00000000
// r1 -> 0x00000C42
// r2 -> 0x00000100 - start of ATAGS
// preserve these registers as argument for kernel
_start:
.globl entry
entry:
	// Clear out bss.
	ldr r4, = edata
	ldr r9, = end
	mov r5, #0
	mov r6, #0
	mov r7, #0
	mov r8, #0
	b	check
 
zero:
	// store multiple at r4.
	stmia r4!, {r5-r8}
 
	// If we are still below bss_end, loop.
check:
	cmp r4, r9
	blo zero
	
	// Turn on the MMU
	// Ref. http://www.embedded-bits.co.uk/2011/mmucode/
	ldr r0, =(entry_pgdir - KERNBASE)
	mcr p15, 0, r0, c2, c0, 0

	mov r0, #0xFFFFFFFF
	mcr p15, 0, r0, c3, c0, 0

	mrc p15, 0, r0, c1, c0, 0
	orr r0, r0, #0x1
	mcr p15, 0, r0, c1, c0, 0
	
	//Jump up above KERNBASE before entering C code
	ldr lr, =relocated
	bx lr

relocated:
	ldr sp, =bootstacktop  // Setup the stack.
	bl arm_init

	// halt
halt:
	wfe
	b halt

.data
// boot stack
         .p2align        20         // force page alignment
         .globl          bootstack
bootstack:
         .space          0x8000
         .globl          bootstacktop
bootstacktop:

