/*
 * Tiny position-independent UniROM/NoPS V2 receiver.
 *
 * The dashboard copies this code to 0x8000C800, switches to its scratch
 * stack, and jumps here. We then clear the dashboard's 0x80010000-0x80200000
 * footprint and receive the incoming padded EXE directly at its final load
 * address. No dashboard function, asset, stack, or staging buffer is needed
 * after sioMiniLoaderStart begins.
 */

.set noreorder
.set noat
.set zero, $zero
.set t0, $t0
.set t1, $t1
.set t2, $t2
.set t3, $t3
.set t5, $t5
.set t6, $t6
.set t7, $t7
.set t8, $t8
.set t9, $t9
.set s0, $s0
.set s1, $s1
.set s2, $s2
.set s3, $s3
.set s4, $s4
.set s5, $s5
.set s6, $s6
.set s7, $s7
.set s8, $s8
.set sp, $sp
.set gp, $gp
.set a0, $a0
.set a1, $a1
.set a2, $a2
.set v0, $v0
.set ra, $ra

.section .text.sioMiniLoader, "ax"
.global sioMiniLoaderStart
.global sioMiniLoaderEnd
.type sioMiniLoaderStart, @function
.type sioMiniLoaderEnd, @function

.set SIO_BASE_HI,    0xbf80
.set SIO_DATA_OFF,   0x1050
.set SIO_STAT_OFF,   0x1054
.set SIO_CTRL_OFF,   0x105a
.set SIO_MODE_OFF,   0x1058
.set SIO_BAUD_OFF,   0x105e
.set HEADER_HI,      0x8000
.set HEADER_LO,      0xd000

sioMiniLoaderStart:
	/* The stack is below this stub and below the parameter block. */
	lui     sp, 0x8000
	ori     sp, sp, 0xc700

	/* Clear the dashboard and all stale staging data. */
	lui     t0, 0x8001
	lui     t1, 0x8020
1:
	sw      zero, 0(t0)
	addiu   t0, t0, 4
	bne     t0, t1, 1b
	nop

	/* UniROM's SIO1 initialization: 115200 baud, 8N2. */
	lui     t0, SIO_BASE_HI
	ori     t1, zero, 0x0040
	sh      t1, SIO_CTRL_OFF(t0)
	ori     t1, zero, 0x0012
	sh      t1, SIO_BAUD_OFF(t0)
	ori     t1, zero, 0x00ce
	sh      t1, SIO_MODE_OFF(t0)
	ori     t1, zero, 0x0005
	sh      t1, SIO_CTRL_OFF(t0)

	/* Wait for SEXE, ignoring any stale TTY character before it. */
2:
	bal     sioMiniGetByte
	nop
	ori     t1, zero, 0x0053       /* S */
	bne     v0, t1, 2b
	nop
	bal     sioMiniGetByte
	nop
	ori     t1, zero, 0x0045       /* E */
	bne     v0, t1, 2b
	nop
	bal     sioMiniGetByte
	nop
	ori     t1, zero, 0x0058       /* X */
	bne     v0, t1, 2b
	nop
	bal     sioMiniGetByte
	nop
	ori     t1, zero, 0x0045       /* E */
	bne     v0, t1, 2b
	nop

	/* OKV2 / UPV2 / OKAY negotiation. */
	ori     a0, zero, 0x004f       /* O */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x004b       /* K */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0056       /* V */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0032       /* 2 */
	bal     sioMiniPutByte
	nop

	bal     sioMiniGetByte
	nop
	ori     t1, zero, 0x0055       /* U */
	bne     v0, t1, sioMiniLoaderStart
	nop
	bal     sioMiniGetByte
	nop
	ori     t1, zero, 0x0050       /* P */
	bne     v0, t1, sioMiniLoaderStart
	nop
	bal     sioMiniGetByte
	nop
	ori     t1, zero, 0x0056       /* V */
	bne     v0, t1, sioMiniLoaderStart
	nop
	bal     sioMiniGetByte
	nop
	ori     t1, zero, 0x0032       /* 2 */
	bne     v0, t1, sioMiniLoaderStart
	nop

	ori     a0, zero, 0x004f       /* O */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x004b       /* K */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0041       /* A */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0059       /* Y */
	bal     sioMiniPutByte
	nop

	/* Read the complete 2048-byte PS-EXE header. */
	lui     s7, HEADER_HI
	ori     s7, s7, HEADER_LO
	ori     t6, zero, 0x0800
3:
	bal     sioMiniGetByte
	nop
	sb      v0, 0(s7)
	addiu   s7, s7, 1
	addiu   t6, t6, -1
	bne     t6, zero, 3b
	nop

	/* Metadata: entry, destination, padded body size, whole-body checksum. */
	bal     sioMiniReadU32
	nop
	move    s0, v0
	bal     sioMiniReadU32
	nop
	move    s3, v0
	bal     sioMiniReadU32
	nop
	move    s4, v0
	bal     sioMiniReadU32
	nop
	move    s5, v0

	/* Validate the actual PS-EXE header, not only NoPS's repeated metadata. */
	lui     t0, HEADER_HI
	ori     t0, t0, HEADER_LO
	lw      t1, 0x00(t0)
	lui     t2, 0x582d
	ori     t2, t2, 0x5350       /* little-endian "PS-X" */
	bne     t1, t2, sioMiniFatal
	nop
	lw      t1, 0x04(t0)
	lui     t2, 0x4558
	ori     t2, t2, 0x4520       /* little-endian " EXE" */
	bne     t1, t2, sioMiniFatal
	nop
	lw      t1, 0x10(t0)
	bne     t1, s0, sioMiniFatal /* repeated entry must match header */
	nop
	lw      t1, 0x18(t0)
	bne     t1, s3, sioMiniFatal /* repeated load address must match */
	nop

	/* Reject addresses outside a conservative stock-PS1 RAM window. */
	lui     t0, 0x8001
	sltu    t1, s3, t0
	bne     t1, zero, sioMiniFatal
	nop
	lui     t0, 0x8020
	sltu    t1, s3, t0
	beq     t1, zero, sioMiniFatal
	nop
	sll     t2, s4, 2
	srl     t2, t2, 2          /* force a harmless overflow check path */
	addu    t3, s3, s4
	sltu    t1, t3, s3
	bne     t1, zero, sioMiniFatal
	nop
	sltu    t1, t0, t3
	bne     t1, zero, sioMiniFatal
	nop
	lui     t0, 0x8001
	sltu    t1, s0, t0
	bne     t1, zero, sioMiniFatal
	nop
	lui     t0, 0x8020
	sltu    t1, s0, t0
	beq     t1, zero, sioMiniFatal
	nop
	andi    t1, s0, 0x0003
	bne     t1, zero, sioMiniFatal
	nop

	move    s8, s4             /* original body size */
	move    s1, zero           /* first progress update not drawn yet */
	move    s2, zero           /* chunks since the last screen update */
	move    s6, zero           /* whole-body checksum */

	/* Receive and checksum one 2048-byte chunk at a time. */
6:
	beq     s4, zero, 9f
	nop
	ori     t6, zero, 0x0800
	sltu    t7, s4, t6
	beq     t7, zero, 7f
	nop
	move    t5, s4
	b       sioMiniChunkReady
	nop
7:
	move    t5, t6
sioMiniChunkReady:
	move    s7, s3             /* retry cursor; s3 stays chunk start */
	move    t7, zero
	move    t8, zero
8:
	bal     sioMiniGetByte
	nop
	sb      v0, 0(s7)
	addiu   s7, s7, 1
	addu    t8, t8, v0
	addiu   t7, t7, 1
	sltu    t9, t7, t5
	bne     t9, zero, 8b
	nop

	/* CHEK and the host's additive chunk checksum. */
	move    s7, t8
	ori     a0, zero, 0x0043       /* C */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0048       /* H */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0045       /* E */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x004b       /* K */
	bal     sioMiniPutByte
	nop
	bal     sioMiniReadU32
	nop
	bne     v0, s7, sioMiniBadChunk
	nop

	addu    s6, s6, s7
	addu    s3, s3, t5
	subu    s4, s4, t5

	/*
	 * Do not draw between metadata and the first body chunk: NoPS streams
	 * that chunk immediately, without waiting for another acknowledgment.
	 * Once its checksum has arrived the host is blocked waiting for MORE,
	 * which is the first safe opportunity to make the bar appear.
	 */
	beq     s1, zero, sioMiniProgressFirst
	nop
	addiu   s2, s2, 1

	/*
	 * At 115200 baud with UniROM's 8N2 framing, ten 2 KiB chunks are about
	 * two seconds. Updating only at that interval keeps GPU polling out of
	 * the transfer's hot path.
	 */
	sltiu   t0, s2, 10
	bne     t0, zero, sioMiniProgressMaybeFinal
	nop
	move    s2, zero
	subu    a0, s8, s4
	bal     sioMiniDrawProgress
	nop
	b       sioMiniProgressDone
	nop
sioMiniProgressFirst:
	ori     s1, zero, 1
	move    s2, zero
	subu    a0, s8, s4
	bal     sioMiniDrawProgress
	nop
	b       sioMiniProgressDone
	nop
sioMiniProgressMaybeFinal:
	bne     s4, zero, sioMiniProgressDone
	nop
	subu    a0, s8, s4
	bal     sioMiniDrawProgress
	nop
sioMiniProgressDone:

	ori     a0, zero, 0x004d       /* M */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x004f       /* O */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0052       /* R */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0045       /* E */
	bal     sioMiniPutByte
	nop
	b       6b
	nop

sioMiniBadChunk:
	ori     a0, zero, 0x0045       /* E */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0052       /* R */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0052       /* R */
	bal     sioMiniPutByte
	nop
	ori     a0, zero, 0x0021       /* ! */
	bal     sioMiniPutByte
	nop
	b       6b
	nop

9:
	bne     s6, s5, sioMiniFatal
	nop

	/* Hardware handoff: the image is already at its final address. */
	lui     t0, 0x1f80
	ori     t1, zero, 0xffff
	sh      t1, 0x1d8c(t0)
	sh      t1, 0x1d8e(t0)
	sh      zero, 0x1d80(t0)
	sh      zero, 0x1d82(t0)
	ori     t1, t0, 0x1088
	ori     t2, zero, 7
10:
	sw      zero, 0(t1)
	addiu   t1, t1, 16
	addiu   t2, t2, -1
	bne     t2, zero, 10b
	nop
	lui     t1, 0x1f80
	ori     t1, t1, 0x1814
	lui     t2, 0x0100
	sw      t2, 0(t1)
	lui     t1, 0x1f80
	ori     t1, t1, 0x1074
	sh      zero, 0(t1)
	addiu   t1, t1, -4
	sh      zero, 0(t1)
	mtc0    zero, $7
	mtc0    zero, $8
	mtc0    zero, $9
	mtc0    zero, $12
	nop

	/* Flush the received code, then launch through the same BIOS Exec path
	 * used by the attached working cdloader. The full PS-EXE header remains
	 * at 0x8000D000, so its Exec buffer begins at +0x10. */
	ori     t1, zero, 0x44
	ori     t2, zero, 0x00a0
	jalr    t2
	nop

	move    s1, zero
	move    s2, zero
	move    s3, zero
	move    s4, zero
	move    s5, zero
	move    s6, zero
	lui     a0, HEADER_HI
	ori     a0, a0, HEADER_LO + 0x10
	move    a1, zero
	move    a2, zero
	ori     t1, zero, 0x43
	ori     t2, zero, 0x00a0
	jr      t2
	nop

sioMiniFatal:
	/* No dashboard remains to render an error. Keep the serial port armed so
	 * a reset/retry is the only required recovery path. */
	b       sioMiniFatal
	nop

/* Blocking SIO1 byte receive. Returns the byte in v0. */
sioMiniGetByte:
	lui     t0, SIO_BASE_HI
11:
	lbu     t1, SIO_STAT_OFF(t0)
	andi    t1, t1, 0x0002
	beq     t1, zero, 11b
	nop
	lhu     t1, SIO_CTRL_OFF(t0)
	ori     t1, t1, 0x0010
	sh      t1, SIO_CTRL_OFF(t0)
	lbu     v0, SIO_DATA_OFF(t0)
	jr      ra
	nop

/* Blocking SIO1 byte transmit. Byte is in a0. */
sioMiniPutByte:
	lui     t0, SIO_BASE_HI
	li      t3, 20002          /* same finite timeout as UniROM's ttyPutc */
12:
	lbu     t1, SIO_STAT_OFF(t0)
	andi    t1, t1, 0x0005
	ori     t2, zero, 0x0005
	beq     t1, t2, sioMiniPutReady
	nop
	addiu   t3, t3, -1
	bne     t3, zero, 12b
	nop
sioMiniPutReady:
	sb      a0, SIO_DATA_OFF(t0)
	jr      ra
	nop

/*
 * Draw/update a proportional progress bar in the final dashboard
 * framebuffer. a0 is the number of body bytes received; s8 is total size.
 * The renderer's drawing origin and display offset are deliberately left
 * intact by quiesceForSioLoader(), so screen-relative coordinates still map
 * to the visible buffer.
 */
sioMiniDrawProgress:
	addiu   sp, sp, -16
	sw      ra, 12(sp)
	sw      a0, 0(sp)

	/* Dark background; drawing it again is harmless and makes the first
	 * update visibly introduce the bar after the header arrives. */
	lui     a0, 0x6030
	ori     a0, a0, 0x3030       /* dark-gray rectangle command/color */
	bal     sioMiniGpuWord
	nop
	beq     v0, zero, sioMiniProgressReturn
	nop
	lui     a0, 0x00a6
	ori     a0, a0, 0x0010       /* x=16, y=166 */
	bal     sioMiniGpuWord
	nop
	beq     v0, zero, sioMiniProgressReturn
	nop
	lui     a0, 0x000a
	ori     a0, a0, 0x0120       /* w=288, h=10 */
	bal     sioMiniGpuWord
	nop
	beq     v0, zero, sioMiniProgressReturn
	nop

	/* received * 284 / total gives the green interior width. */
	lw      t1, 0(sp)
	ori     t2, zero, 284
	multu   t1, t2
	mflo    t1
	beq     s8, zero, sioMiniProgressWidthReady
	move    t2, zero
	divu    t1, s8
	mflo    t2
sioMiniProgressWidthReady:
	beq     t2, zero, sioMiniProgressReturn
	nop
	sw      t2, 4(sp)            /* sioMiniGpuWord uses t2 as a mask */

	lui     a0, 0x6060
	ori     a0, a0, 0xc060       /* green rectangle command/color */
	bal     sioMiniGpuWord
	nop
	beq     v0, zero, sioMiniProgressReturn
	nop
	lui     a0, 0x00a8
	ori     a0, a0, 0x0012       /* x=18, y=168 */
	bal     sioMiniGpuWord
	nop
	beq     v0, zero, sioMiniProgressReturn
	nop
	lui     a0, 0x0006
	lw      t2, 4(sp)
	or      a0, a0, t2           /* w=calculated, h=6 */
	bal     sioMiniGpuWord
	nop

sioMiniProgressReturn:
	lw      ra, 12(sp)
	addiu   sp, sp, 16
	jr      ra
	nop

/*
 * Wait for GPUSTAT bit 26 ("ready for command") and write one GP0 word.
 * Rendering is diagnostic only, so a wedged GPU times out instead of ever
 * being allowed to stop the serial transfer. Returns v0=1 on success.
 */
sioMiniGpuWord:
	lui     t0, 0xbf80
	ori     t0, t0, 0x1810
	lui     t3, 0x0001
sioMiniGpuWait:
	lw      t1, 4(t0)
	lui     t2, 0x0400
	and     t1, t1, t2
	bne     t1, zero, sioMiniGpuReady
	nop
	addiu   t3, t3, -1
	bne     t3, zero, sioMiniGpuWait
	nop
	move    v0, zero
	jr      ra
	nop
sioMiniGpuReady:
	sw      a0, 0(t0)
	ori     v0, zero, 1
	jr      ra
	nop

/* Read a little-endian uint32 into v0. */
sioMiniReadU32:
	addiu   sp, sp, -8
	sw      ra, 4(sp)
	move    t8, zero
	move    t6, zero
	move    t7, zero
13:
	bal     sioMiniGetByte
	nop
	sllv    t9, v0, t7
	or      t8, t8, t9
	addiu   t6, t6, 1
	addiu   t7, t7, 8
	slti    t9, t6, 4
	bne     t9, zero, 13b
	nop
	move    v0, t8
	lw      ra, 4(sp)
	addiu   sp, sp, 8
	jr      ra
	nop

sioMiniLoaderEnd:
	.size sioMiniLoaderStart, sioMiniLoaderEnd - sioMiniLoaderStart

/* Code starts at 0x8000C800 and the header starts at 0x8000D000. Refuse to
 * assemble if a future edit makes those two scratch regions overlap. */
.if (sioMiniLoaderEnd - sioMiniLoaderStart) > 0x800
	.error "SIO mini-loader exceeds the 0x800-byte C800-D000 code window"
.endif
