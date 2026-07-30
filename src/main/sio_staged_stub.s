/*
 * Position-independent final SIO handoff.
 *
 * Parameters at 0x8000C7C0:
 *   +0 destination, +4 staged source, +8 word count,
 *   +12 entry PC, +16 GP, +20 SP.
 *
 * Unlike the original stable trampoline, this version handles overlapping
 * source/destination ranges in either direction before jumping directly with
 * the PS-EXE register contract.
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
.set gp, $gp

.section .text.sioStagedStub, "ax"
.global sioStagedStubStart
.global sioStagedStubEnd
.type sioStagedStubStart, @function
.type sioStagedStubEnd, @function

sioStagedStubStart:
	lui     t9, 0x8000
	ori     t9, t9, 0xc7c0
	lw      a0, 0(t9)
	lw      a1, 4(t9)
	lw      a2, 8(t9)
	lw      s0, 12(t9)
	lw      s1, 16(t9)
	lw      s2, 20(t9)

	/* If source < destination, copy backwards. */
	sltu    t0, a1, a0
	beq     t0, zero, sioStubForward
	nop
	sll     t1, a2, 2
	addu    t0, a1, t1
	addu    t1, a0, t1
	addiu   t0, t0, -4
	addiu   t1, t1, -4
sioStubBackward:
	beq     a2, zero, sioStubDone
	nop
	lw      t2, 0(t0)
	sw      t2, 0(t1)
	addiu   t0, t0, -4
	addiu   t1, t1, -4
	addiu   a2, a2, -1
	b       sioStubBackward
	nop

sioStubForward:
	beq     a2, zero, sioStubDone
	nop
	lw      t2, 0(a1)
	sw      t2, 0(a0)
	addiu   a1, a1, 4
	addiu   a0, a0, 4
	addiu   a2, a2, -1
	b       sioStubForward
	nop

sioStubDone:
	/* Flush the received code before entering it. */
	ori     t1, zero, 0x44
	ori     t2, zero, 0x00a0
	jalr    t2
	nop
	move    gp, s1
	move    sp, s2
	jr      s0
	nop

sioStagedStubEnd:
	.size sioStagedStubStart, sioStagedStubEnd - sioStagedStubStart
