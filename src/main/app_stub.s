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

# ---------------------------------------------------------------------------
# Progress marks.
#
# Stage 1 has now failed on three separate targets while the direct copy-and-
# jump has worked on three others, and every failure has looked identical from
# the outside: a black screen with no way to tell which step died. This is the
# same trick that made the standalone SIO loader debuggable - paint the whole
# screen a flat colour at each phase, so whatever colour is left tells you
# exactly how far it got.
#
# quiesceForHandoff() issues GP1(00) which turns the display off, so the mark
# macro re-enables it every time rather than assuming any GPU state. Written
# with raw register pokes and no absolute addresses so the stub stays position
# independent.
#
#   red      entered stage 1, about to copy
#   blue     payload copied
#   yellow   zero-fills done
#   white    BIOS FlushCache returned, about to jump
#
# A colour left on screen means that step completed and the NEXT one hung.
# Reaching white and stopping means the target itself did not start.
# Set APP_STUB_MARKS to 0 to compile them out once the path is trusted.
.set APP_STUB_MARKS, 1

.macro mark colour
.if APP_STUB_MARKS
	lui     $t0, 0xbf80
	ori     $t1, $zero, 0x0300          # GP1(03) display enable
	sll     $t1, $t1, 16
	sw      $t1, 0x1814($t0)
	ori     $t1, $zero, 0x0800          # GP1(08) 320x240 NTSC 15bpp
	sll     $t1, $t1, 16
	ori     $t1, $t1, 0x0001
	sw      $t1, 0x1814($t0)
	lui     $t1, 0x0600                 # GP1(06) horizontal range
	ori     $t1, $t1, 0x0260
	ori     $t2, $zero, 0xc6
	sll     $t2, $t2, 16
	or      $t1, $t1, $t2
	sw      $t1, 0x1814($t0)
	lui     $t1, 0x0704                 # GP1(07) vertical range
	ori     $t1, $t1, 0x0010
	sw      $t1, 0x1814($t0)
	lui     $t1, 0x0200                 # GP0(02) VRAM fill, colour
	ori     $t1, $t1, \colour
	sw      $t1, 0x1810($t0)
	sw      $zero, 0x1810($t0)          # at (0,0)
	lui     $t1, 0x00f0                 # 320 wide, 240 high
	ori     $t1, $t1, 0x0140
	sw      $t1, 0x1810($t0)
.endif
.endm
# ---------------------------------------------------------------------------

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

	mark    0x0040                /* red: entered stage 1 */

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
	mark    0x4000                /* blue: payload copied */

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
	mark    0x0044                /* yellow: zero-fills done */

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

	mark    0x7fff                /* white: FlushCache returned */

	/* PS-EXE register contract, then straight in. No memory is touched
	 * after the BIOS call - only $s0/$s1/$s2, which the ABI protects. */
	move    $sp, $s2
	move    $gp, $s1
	jr      $s0
	nop

appStubEnd:
	.size appStubStart, appStubEnd - appStubStart
