/*
 * Rebuild the retail BIOS kernel state before replacing this executable.
 *
 * NOT BUILT AND NOT CALLED. bios_reinit.c is not in CMakeLists.txt's source
 * list, and it will not link as it stands: it needs ten BIOS call wrappers
 * (biosEnterCriticalSection, biosSetConf, biosInstallDevices, biosSetMemSize
 * and so on) that src/main/bios_calls.s does not define - that file only
 * provides biosFlushCache, biosOpen, biosClose, biosAddDevice and
 * biosRemoveDevice.
 *
 * Finishing it means writing those wrappers against the correct A0/B0/C0
 * table indices. Getting an index wrong calls an arbitrary kernel routine, so
 * it is not something to do speculatively - see docs/TWO-STAGE-LAUNCHER.md
 * for when this is worth picking up.
 *
 * This is the same warm-boot reconstruction sequence used by the attached,
 * proven cdloader.exe source.  In particular, it replaces the FCB/DCB tables
 * instead of trying to close dashboard-owned descriptors through callbacks
 * that a cartridge may already have hooked.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void reinitializeBIOSForHandoff(void);

#ifdef __cplusplus
}
#endif
