/*
 * Position-independent final handoff stub.
 *
 * This code is copied to BIOS/kernel scratch RAM before an embedded PS-EXE
 * is copied. It then performs the copy while the dashboard is no longer
 * needed, clears all main RAM outside the target image, flushes the cache,
 * and starts it through BIOS A0(43h) Exec. No call into the dashboard may
 * be made after this stub starts.
 */

.set noreorder
.set noat
.set zero, $zero
.set t0, $t0
.set t1, $t1
.set t2, $t2
.set t9, $t9
.set s0, $s0
.set s1, $s1
.set s2, $s2
.set sp, $sp
.set a0, $a0
.set a1, $a1
.set a2, $a2

.section .text.handoffStub, "ax"
.global handoffStubStart
.global handoffStubEnd
.type handoffStubStart, @function
.type handoffStubEnd, @function

handoffStubStart:
	/* Parameter block at 0x8000C7C0:
	 *   +0 source payload, +4 destination, +8 word count.
	 * BIOS Exec data is held separately at 0x8000C740. */
	lui     t9, 0x8000
	ori     t9, t9, 0xc7c0
	lw      s0, 0(t9)             /* source */
	lw      s1, 4(t9)             /* destination */
	lw      s2, 8(t9)             /* word count */
	lui     sp, 0x8000
	ori     sp, sp, 0xc700

	/* If source < destination, copy backwards (memmove semantics). */
	sltu    t0, s0, s1
	beq     t0, zero, 1f
	nop
	sll     t1, s2, 2
	addu    t0, s0, t1
	addu    t1, s1, t1
	addiu   t0, t0, -4
	addiu   t1, t1, -4
2:
	beq     s2, zero, 4f
	nop
	lw      t2, 0(t0)
	sw      t2, 0(t1)
	addiu   t0, t0, -4
	addiu   t1, t1, -4
	addiu   s2, s2, -1
	b       2b
	nop

	/* Source is above/equal to destination: copy forwards. */
1:
	beq     s2, zero, 4f
	nop
	lw      t2, 0(s0)
	sw      t2, 0(s1)
	addiu   s0, s0, 4
	addiu   s1, s1, 4
	addiu   s2, s2, -1
	b       1b
	nop

	/* Preserve the target range before clearing the rest of RAM. */
4:
	lw      s1, 4(t9)
	lw      t0, 8(t9)
	sll     t0, t0, 2
	addu    s2, s1, t0         /* target end */

	/* Clear from the application origin up to the target start. */
	lui     t0, 0x8001
	sltu    t1, t0, s1
	beq     t1, zero, 6f
	nop
5:
	sw      zero, 0(t0)
	addiu   t0, t0, 4
	sltu    t1, t0, s1
	bne     t1, zero, 5b
	nop

	/* Clear from the target end to the top of a stock 2 MB PS1. */
6:
	lui     t0, 0x8020
	sltu    t1, s2, t0
	beq     t1, zero, 8f
	nop
7:
	sw      zero, 0(s2)
	addiu   s2, s2, 4
	sltu    t1, s2, t0
	bne     t1, zero, 7b
	nop

	/* Silence SPU, stop every DMA channel, reset GPU and mask IRQs. */
8:
	lui     t0, 0x1f80
	ori     t1, zero, 0xffff
	sh      t1, 0x1d8c(t0)       /* SPU_KOFF0 */
	sh      t1, 0x1d8e(t0)       /* SPU_KOFF1 */
	sh      zero, 0x1d80(t0)     /* SPU_MVOLL */
	sh      zero, 0x1d82(t0)     /* SPU_MVOLR */

	ori     t1, t0, 0x1088       /* DMA_CHCR(0) */
	ori     t2, zero, 7
9:
	sw      zero, 0(t1)
	addiu   t1, t1, 16
	addiu   t2, t2, -1
	bne     t2, zero, 9b
	nop

	lui     t1, 0x1f80
	ori     t1, t1, 0x1814
	lui     t2, 0x0100
	sw      t2, 0(t1)             /* GP1(00), software reset */

	lui     t1, 0x1f80
	ori     t1, t1, 0x1074
	sh      zero, 0(t1)           /* IRQ_MASK */
	addiu   t1, t1, -4
	sh      zero, 0(t1)           /* IRQ_STAT at 0x1070 */

	mtc0    zero, $7              /* DCIC */
	mtc0    zero, $8              /* BDA */
	mtc0    zero, $9              /* BDAM */
	mtc0    zero, $12             /* CPU interrupts disabled */
	nop

	/* BIOS A0(44h) = FlushCache after writing the executable body. */
	ori     t1, zero, 0x44
	ori     t2, zero, 0x00a0
	jalr    t2
	nop

	/*
	 * Match the attached working cdloader exactly: zero s1-s6, then invoke
	 * BIOS A0(43h) Exec with the header-offset buffer. Exec applies PC, GP,
	 * BSS and stack fields using the normal PS-EXE launch semantics.
	 */
	move    s1, zero
	move    s2, zero
	move    $s3, zero
	move    $s4, zero
	move    $s5, zero
	move    $s6, zero
	lui     a0, 0x8000
	ori     a0, a0, 0xc740
	move    a1, zero
	move    a2, zero
	ori     t1, zero, 0x43
	ori     t2, zero, 0x00a0
	jr      t2
	nop

handoffStubEnd:
	.size handoffStubStart, handoffStubEnd - handoffStubStart
