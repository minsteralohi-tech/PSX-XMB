/*
 * PSX-iTests - UniROM-compatible serial TTY redirect
 *
 * Installs a replacement BIOS "tty" device that shifts all TTY output
 * (printf / std_out / anything written to the tty device) out of the SIO1
 * serial port, using the exact same mechanism UniROM uses on startup:
 * delete the current tty device BY NAME, then install our own in its place.
 *
 * Because removal is keyed on the name string "tty" (not on a specific
 * handler pointer), this stays compatible with UniROM and any other homebrew
 * that does the same delete-and-replace: whoever installs last owns tty, and
 * there is never more than one active tty, so output never doubles. When
 * UniROM later loads it simply RemoveDevice("tty")s ours and installs its
 * own - exactly as intended.
 *
 * See UNIROM_TTY_SERIAL_README.md for the full derivation.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Configure SIO1 and swap in our tty device (delete-by-name + add), then print
// "Replacement Shell by Hidden0 Loaded." over the freshly installed driver.
// Call once at startup.
void installSerialTTY(void);

/* Remove our TTY device from the kernel's device table again. Must be done
 * before handing the console to a program that may overwrite this image -
 * see the long note in tty_serial.c. Safe to call if it was never installed. */
void uninstallSerialTTY(void);

// Write a string straight out of the installed SIO1 tty (also used internally
// for the startup banner).
void ttySerialWrite(const char *str);

#ifdef __cplusplus
}
#endif
