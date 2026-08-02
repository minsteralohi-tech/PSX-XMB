# PSX-iTests - BIOS B0 call trampolines for the TTY device swap
#
# The PS1 kernel exposes its call tables at fixed addresses 0xA0/0xB0/0xC0: to
# invoke function <n> of table B you load the index into $t1 and jump to 0xB0,
# where a dispatcher tail-calls the real routine. Because we enter these stubs
# via `jal` from C, $ra already holds the C return address; we jump (not link)
# to 0xB0 so the dispatcher's final `jr $ra` returns straight back to the C
# caller with the result in $v0. Arguments are already in $a0.. per the o32 ABI.
#
# This is the same documented device-table API UniROM uses (see the TTY readme):
#   B0(0x47) AddDevice(DCB*)         B0(0x48) RemoveDevice(char *name)
#   B0(0x32) open(name, mode)        B0(0x36) close(fd)

.set noreorder
.set noat

.section .text, "ax", @progbits

.macro defBiosB0 name, num
.global \name
.type \name, @function
\name:
	li    $t2, 0xb0
	jr    $t2
	li    $t1, \num       # branch-delay slot: set the call index
.size \name, . - \name
.endm

defBiosB0 biosAddDevice,    0x47
defBiosB0 biosRemoveDevice, 0x48
defBiosB0 biosOpen,         0x32
defBiosB0 biosClose,        0x36
defBiosB0 biosRead,         0x34
defBiosB0 biosLseek,        0x33

# --- A0 table calls (same idea, jump to 0xA0 with the index in $t1) -------
# FlushCache (A0 0x44) must run before jumping into a freshly-received program
# so the I-cache doesn't execute stale bytes from whatever was there before.
.macro defBiosA0 name, num
.global \name
.type \name, @function
\name:
	li    $t2, 0xa0
	jr    $t2
	li    $t1, \num        # branch-delay slot: set the call index
.size \name, . - \name
.endm

defBiosA0 biosFlushCache,   0x44

# _96_init (A0 0x54) brings up the kernel's CD-ROM filesystem device, the one
# that backs "cdrom:" paths in open()/read(). Without it those calls fail on
# real hardware even though an emulator may let them through.
defBiosA0 bios96Init,       0x54

# _96_remove (A0 0x56) tears it back down again.
defBiosA0 bios96Remove,     0x56

# Exec (A0 0x43) is how the BIOS itself starts a PS-EXE. Given a pointer to the
# ten-word EXEC structure - which is exactly the PS-EXE header from offset 0x10
# onwards - it sets $gp, sets up the stack and frame pointer from s_addr/s_size,
# zero-fills b_addr/b_size and calls pc0, all with the kernel live.
#
# This exists for targets that are unhappy with a hand-rolled jump. UniROM is
# the case in point: it installs its own kernel exception handler and TTY
# redirect and leans on BIOS services from its first instruction, and it starts
# correctly when the BIOS boots it but not when we set the registers ourselves.
# Bare-metal programs such as the 240p suite and the standalone SIO loader do
# not care either way and keep using the direct jump.
#
# Returns to the caller (with 0 in $v0) only if the launch failed.
defBiosA0 biosExec,         0x43
