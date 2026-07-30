# ps1-ram-tester - (C) 2026 spicyjpeg
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
# REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
# OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.

.set noreorder

# Was 2048 - raised to 8192. This project has grown substantially deeper
# call chains and larger per-call locals than the original 2KB budget was
# sized for (GTE-heavy theme rendering, nested menu navigation, snprintf
# formatting, etc), and a real, reported "random crash specifically after
# using the heaviest themes" pattern is a classic stack-overflow signature -
# corruption depends on exact call depth/local layout, hence "random".
# _stackBuffer below is deliberately placed in .sbss (GP-relative
# addressed, see the %gprel() reference a few lines down), which has to
# stay within about +/-32KB of $gp for that reference to still assemble -
# see handoff.h's/executable.ld's notes on this exact class of bug for the
# full story. 8KB leaves that comfortably intact while still being a
# meaningful improvement; main RAM itself has roughly 450KB of headroom, so
# the extra 6KB here costs nothing that matters.
.set STACK_SIZE, 8192

# We're going to override ps1-bare-metal's _start() with a minimal version that
# moves the stack to a statically allocated buffer, allowing for the end of main
# RAM to be safely tested. Note that this implementation will not invoke global
# constructors and destructors.
.section .text._start, "ax", @progbits
.global _start
.type _start, @function

_start:
	la    $gp, _gp
	addiu $sp, $gp, %gprel(_stackBuffer) + STACK_SIZE - 16

	# _bssStart/_bssEnd bracket the WHOLE regular .bss section (every large
	# static array in the program, as opposed to the small stuff in .sbss
	# that $gp is anchored near) - so as the project's static buffers grow,
	# the distance from $gp to _bssEnd grows with them. %gprel() encodes
	# that distance as a single 16-bit signed relocation (R_MIPS_GPREL16),
	# which only reaches ±32KB from $gp. This project just grew past that:
	#
	#   ld: small-data section too large; lower small-data size limit
	#   relocation truncated to fit: R_MIPS_GPREL16 against `_bssEnd'
	#
	# Fixed by loading both as plain 32-bit absolute addresses (`la` here
	# expands to lui+ori, not a single gprel16 instruction) instead of
	# GP-relative - one extra instruction each, paid exactly once at boot,
	# with no size ceiling to eventually outgrow again.
	la    $a0, _bssStart
	la    $a2, _bssEnd
	subu  $a2, $a0
	jal   memset
	li    $a1, 0

	j     main
	nop

.section .sbss._stackBuffer, "aw"
.type _stackBuffer, @object

_stackBuffer:
	.space STACK_SIZE
