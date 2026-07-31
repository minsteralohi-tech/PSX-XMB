/*
 * PSX-iTests - UniROM-compatible serial TTY redirect (see tty_serial.h)
 *
 * Reproduces UniROM's startup behaviour byte-for-byte at the mechanism level:
 *
 *   serial_init();               // SIO1: CTRL=0x40, BAUD=0x12, MODE=0xCE, CTRL=0x05
 *   <interrupts disabled>
 *   close(0); close(1);          // drop stdin / stdout
 *   RemoveDevice("tty");         // delete whatever tty is installed, BY NAME
 *   AddDevice(&ourDCB);          // install ours (action handler -> SIO1)
 *   open("tty", 2);  -> fd 0
 *   open("tty", 1);  -> fd 1
 *   <interrupts restored>
 *
 * We do NOT use UniROM's 0x0000C000 relocation trick - that only exists so its
 * handlers survive independently of where its code is loaded. Our handlers live
 * at stable addresses in this .exe, so the DCB points straight at them (exactly
 * as the readme's §10 "minimal functional replica" recommends).
 */

#include <stdint.h>
#include <stdbool.h>
#include "main/tty_serial.h"

/* ---- SIO1 registers (KSEG1 / uncached, as the readme requires) ---------- */

#define SIO1_DATA (*(volatile uint8_t  *) 0xBF801050)
#define SIO1_STAT (*(volatile uint8_t  *) 0xBF801054)
#define SIO1_MODE (*(volatile uint16_t *) 0xBF801058)
#define SIO1_CTRL (*(volatile uint16_t *) 0xBF80105A)
#define SIO1_BAUD (*(volatile uint16_t *) 0xBF80105E)

/* Exact §7 init sequence and values used by UniROM's boot-time SIO1 setup. */
static void serialInit(void) {
	SIO1_CTRL = 0x0040;   // reset / acknowledge internal state
	SIO1_BAUD = 0x0012;   // baud reload -> ~115200
	SIO1_MODE = 0x00CE;   // 8 data bits, no parity, 2 stop bits, MUL16
	SIO1_CTRL = 0x0005;   // TXEN | RXEN
}

/* Hardware-proven build behavior: proceed when either low ready bit is set,
 * with the same 20002 timeout so we
 * never hang if nothing is listening on the other end. */
static void serialPutc(uint8_t c) {
	unsigned timeout = 20002;

	while ((SIO1_STAT & 0x05) == 0) {
		if (--timeout == 0)
			break;
	}

	SIO1_DATA = c;   // byte store
}

static void serialWrite(const char *buf, int len) {
	for (int i = 0; i < len; i++)
		serialPutc((uint8_t) buf[i]);
}

void ttySerialWrite(const char *str) {
	int len = 0;
	while (str[len])
		len++;
	serialWrite(str, len);
}

/* ---- BIOS B0 calls (trampolines in bios_calls.s) ------------------------ */

extern int biosAddDevice(const void *dcb);       // B0(0x47)
extern int biosRemoveDevice(const char *name);   // B0(0x48)
extern int biosOpen(const char *name, int mode); // B0(0x32)
extern int biosClose(int fd);                    // B0(0x36)

/* ---- Device Control Block + handlers ------------------------------------ */

/*
 * The kernel dispatches all character I/O for this device through the single
 * `action` entry (+0x18) with an operation selector, NOT through the separate
 * read/write pointers - which therefore point at a do-nothing stub. This is
 * the shape of the stock SIO tty device that the readme (§5/§6) verified.
 *
 * action calling convention (§6):
 *   a0 = request struct   [a0+0x08] = buffer pointer   [a0+0x0C] = length
 *   a1 = operation selector,  2 = write
 */
static int ttyAction(void *req, int op) {
	if (op == 2) {   // write
		const char *buf = *(const char *const *)((const char *) req + 0x08);
		int         len = *(const int *)         ((const char *) req + 0x0C);
		serialWrite(buf, len);
		return len;
	}

	// Read / other ops: nothing buffered, report "no data".
	return 0;
}

// Stubs for every entry that does nothing (open/close/ioctl return 0; the rest
// are init/read/write/... no-ops). Kept as real functions so the DCB has stable
// addresses to point at.
static int ttyStub(void) {
	return 0;
}

// Layout matches the DCB in readme §5 exactly (name first, then flags,
// blocksize, desc, and the 15 handler pointers). Non-const because AddDevice
// registers this pointer into the kernel's live RAM device table.
typedef struct {
	const char *name;
	uint32_t    flags;
	uint32_t    blocksize;
	const char *desc;
	void       *init;
	void       *open;
	void       *action;
	void       *close;
	void       *ioctl;
	void       *read;
	void       *write;
	void       *erase;
	void       *undelete;
	void       *firstfile;
	void       *nextfile;
	void       *format;
	void       *deinit;
	void       *check;
} DeviceControlBlock;

static const char ttyName[] = "tty";
static const char ttyDesc[] = "SIO_TTY";

static DeviceControlBlock ttyDCB = {
	.name      = ttyName,
	.flags     = 0x00000003,
	.blocksize = 0x00000001,
	.desc      = ttyDesc,
	.init      = (void *) ttyStub,
	.open      = (void *) ttyStub,
	.action    = (void *) ttyAction,   // the only real handler (+0x18)
	.close     = (void *) ttyStub,
	.ioctl     = (void *) ttyStub,
	.read      = (void *) ttyStub,
	.write     = (void *) ttyStub,
	.erase     = (void *) ttyStub,
	.undelete  = (void *) ttyStub,
	.firstfile = (void *) ttyStub,
	.nextfile  = (void *) ttyStub,
	.format    = (void *) ttyStub,
	.deinit    = (void *) ttyStub,
	.check     = (void *) ttyStub
};

/* ---- Critical section (raw COP0, as UniROM does - not a BIOS call) ------- */

// Clear the Status register's IEc bit (bit 0) to disable interrupts during the
// device-table edit, returning the previous Status so it can be restored.
static uint32_t irqDisable(void) {
	uint32_t sr;
	__asm__ volatile("mfc0 %0, $12\n nop\n" : "=r"(sr));
	__asm__ volatile("mtc0 %0, $12\n nop\n" :: "r"(sr & ~1u));
	return sr;
}

static void irqRestore(uint32_t sr) {
	__asm__ volatile("mtc0 %0, $12\n nop\n" :: "r"(sr));
}

/* ---- Install ------------------------------------------------------------ */

static bool ttyInstalled = false;

/*
 * Take our TTY device back out of the kernel's device table.
 *
 * This matters far more than it looks. ttyDCB and every function pointer in
 * it live inside this dashboard's own image, around 0x8001xxxx. A launched
 * program that loads over that range - the 240p Test Suite loads at
 * 0x80010000 - replaces those bytes with its own code while the kernel's
 * device table still points at them. The next BIOS call that touches "tty"
 * then jumps into the middle of the launched program, which looks exactly
 * like the program failing to start.
 *
 * That is why cdloader (0x801EA300), the standalone SIO loader (0x801B0000)
 * and UniROM (0x801D0000) all survived the same hand-off: they load above the
 * dashboard, so the dangling pointers still happen to point at intact code.
 *
 * Removing the device by name restores whatever the kernel had before, which
 * is the state a freshly booted program expects.
 */
void uninstallSerialTTY(void) {
	if (!ttyInstalled)
		return;

	biosRemoveDevice(ttyName);
	ttyInstalled = false;
}

void installSerialTTY(void) {
	serialInit();

	// Do the remove/add with interrupts disabled, exactly as UniROM does, so
	// nothing tries to print through a half-swapped device table.
	uint32_t sr = irqDisable();

	biosClose(0);                 // drop stdin
	biosClose(1);                 // drop stdout
	biosRemoveDevice(ttyName);    // delete whatever "tty" is installed, BY NAME
	biosAddDevice(&ttyDCB);       // install ours
	ttyInstalled = true;
	biosOpen(ttyName, 2);         // re-bind -> fd 0
	biosOpen(ttyName, 1);         // re-bind -> fd 1

	irqRestore(sr);

	// The driver is now fully installed and stdin/stdout are bound to it.
	// Announce over the freshly-configured SIO1 tty.
	ttySerialWrite("Replacement Shell by Hidden0 Loaded.\n");
}
