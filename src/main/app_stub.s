/*
 * PSX-iTests - stage 1 of the two-stage launcher (see app_launch.h).
 *
 * Entered with:
 *   $t8 = parameter block address
 *   $t9 = this code's address (only the jump into it needs this)
 *
 * Parameter block, eight words:
 *   +0  destination     +4  source        +8  word count
 *   +12 entry PC        +16 GP            +20 SP
 *   +24 zero-fill list  +28 scratch stack top
 *
 * The zero-fill list is an array of (start, wordCount) pairs terminated by a
 * zero word count.
 *
 * Position independent: no absolute addresses, no relocations, no data
 * outside the parameter block. app_launch.c copies it into whichever arena it
 * picked and jumps.
 *
 * $s7 holds the parameter pointer throughout. The o32 ABI lets a callee
 * clobber $t0-$t9 and the BIOS does, so nothing needed after the FlushCache
 * call may live in a temporary.
 */

.set noreorder
.set noat

.section .text.appStub, "ax", @progbits
.global appStubStart
.global appStubEnd
.type appStubStart, @function

appStubStart:
	move    $s7, $t8

	/* Everything still needed after the BIOS call is loaded NOW, into
	 * callee-saved registers. The proven trampoline in the standalone SIO
	 * loader does exactly this; an earlier version of this file kept $s7
	 * live across the FlushCache call and reloaded the entry point through
	 * it afterwards, which only works if the BIOS honours the o32 ABI for
	 * every register - a bet with no upside. */
	lw      $s0, 12($s7)          /* entry PC      */
	lw      $s1, 16($s7)          /* GP            */
	lw      $s2, 20($s7)          /* SP            */
	lw      $s3, 28($s7)          /* scratch stack */

	/* ---- 1. move the payload into place ---------------------------- */
	lw      $a0,  0($s7)          /* destination */
	lw      $a1,  4($s7)          /* source      */
	lw      $a2,  8($s7)          /* word count  */

	beq     $a2, $zero, appFills
	nop
	beq     $a0, $a1, appFills
	nop

	/* memmove semantics: when the source is below the destination the
	 * ranges may overlap forwards, so copy from the top down. The embedded
	 * blob lives inside the dashboard's .rodata and the destination is
	 * frequently below it, so both directions really do get used. */
	sltu    $t0, $a1, $a0
	beq     $t0, $zero, appCopyForward
	nop

	sll     $t1, $a2, 2
	addu    $t0, $a1, $t1
	addu    $t1, $a0, $t1
	addiu   $t0, $t0, -4
	addiu   $t1, $t1, -4
appCopyBackward:
	lw      $t2, 0($t0)
	sw      $t2, 0($t1)
	addiu   $t0, $t0, -4
	addiu   $t1, $t1, -4
	addiu   $a2, $a2, -1
	bne     $a2, $zero, appCopyBackward
	nop
	b       appFills
	nop

appCopyForward:
	lw      $t2, 0($a1)
	sw      $t2, 0($a0)
	addiu   $a1, $a1, 4
	addiu   $a0, $a0, 4
	addiu   $a2, $a2, -1
	bne     $a2, $zero, appCopyForward
	nop

	/* ---- 2. zero-fill every range in the list ----------------------- *
	 * This is where the dashboard gets erased, and where the target's BSS
	 * gets its PS-EXE memfill. app_launch.c has already proven that no
	 * range covers the payload just copied, this code, or the parameter
	 * and list blocks it is reading from. */
appFills:
	lw      $a3, 24($s7)          /* fill list pointer */
	beq     $a3, $zero, appFinish
	nop

appFillNext:
	lw      $a0, 0($a3)           /* start      */
	lw      $a1, 4($a3)           /* word count */
	beq     $a1, $zero, appFinish
	nop
	addiu   $a3, $a3, 8

appFillLoop:
	sw      $zero, 0($a0)
	addiu   $a0, $a0, 4
	addiu   $a1, $a1, -1
	bne     $a1, $zero, appFillLoop
	nop
	b       appFillNext
	nop

	/* ---- 3. hand over ---------------------------------------------- */
appFinish:
	/*
	 * Switch to a scratch stack at the top of the arena before calling the
	 * BIOS. Two stacks are unusable here:
	 *
	 *   - the dashboard's, because it is a static buffer inside the image
	 *     and the fills above may just have zeroed it;
	 *   - the target's, because it may point into this arena. The 240p
	 *     suite asks for 0x801ffff0 and the arena is 0x801ff000-0x80200000,
	 *     so the BIOS pushing onto the target's stack would be writing into
	 *     the code currently executing. FlushCache is unlikely to use the
	 *     4 KB needed to actually reach it, but "unlikely" is not a basis
	 *     for a hand-off.
	 *
	 * The scratch stack sits above the fill list with ~1.7 KB of room, and
	 * becomes irrelevant the moment the jump below happens.
	 */
	move    $sp, $s3

	/* BIOS A(44h) FlushCache. The payload was written as data, so the
	 * instruction cache may still hold the dashboard's code for those
	 * addresses. */
	ori     $t1, $zero, 0x44
	ori     $t2, $zero, 0x00a0
	jalr    $t2
	nop

	/* PS-EXE register contract, then straight in. No memory is touched
	 * after the BIOS call - only $s0/$s1/$s2, which the ABI protects. */
	move    $sp, $s2
	move    $gp, $s1
	jr      $s0
	nop

appStubEnd:
	.size appStubStart, appStubEnd - appStubStart
