/*
 * Rebuild the retail BIOS kernel state before replacing this executable.
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
