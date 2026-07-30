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
