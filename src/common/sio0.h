/*
 * ps1-bare-metal - (C) 2023-2025 spicyjpeg
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
	SIO0_ADDR_CONTROLLER  = 0x01,
	SIO0_ADDR_MEMORY_CARD = 0x81
} SIO0DeviceAddress;

typedef enum {
	// Controller commands
	SIO0_PAD_POLL        = 'B', // Read controller state
	SIO0_PAD_CONFIG_MODE = 'C', // Enter or exit configuration mode

	// Configuration mode commands
	SIO0_CFG_INIT_PRESSURE  = '@', // Initialize DualShock 2 pressure sensors
	SIO0_CFG_SET_ANALOG     = 'D', // Set analog mode/LED state
	SIO0_CFG_GET_ANALOG     = 'E', // Get analog mode/LED state
	SIO0_CFG_GET_ACT_INFO   = 'F', // Get information about a feedback actuator
	SIO0_CFG_GET_ACT_LIST   = 'G', // Get list of all feedback actuators
	SIO0_CFG_GET_ACT_STATE  = 'H', // Get current state of feedback actuators
	SIO0_CFG_GET_MODE       = 'L', // Get list of all supported modes
	SIO0_CFG_REQUEST_SETUP  = 'M', // Configure poll request format
	SIO0_CFG_RESPONSE_SETUP = 'O', // Configure poll response format

	// Memory card commands
	SIO0_CARD_READ       = 'R', // Read 128-byte sector
	SIO0_CARD_GET_SIZE   = 'S', // Retrieve size information
	SIO0_CARD_WRITE      = 'W'  // Write 128-byte sector
} SIO0DeviceCommand;

typedef enum {
	PAD_TYPE_NONE         =  0,
	PAD_TYPE_MOUSE        =  1,
	PAD_TYPE_NEGCON       =  2,
	PAD_TYPE_IRQ10_GUN    =  3,
	PAD_TYPE_DIGITAL      =  4,
	PAD_TYPE_ANALOG_STICK =  5,
	PAD_TYPE_GUNCON       =  6,
	PAD_TYPE_ANALOG       =  7,
	PAD_TYPE_MULTITAP     =  8,
	PAD_TYPE_JOGCON       = 14,
	PAD_TYPE_CONFIG_MODE  = 15
} ControllerType;

typedef enum {
	PAD_BTN_SELECT   = 1 <<  0,
	PAD_BTN_L3       = 1 <<  1,
	PAD_BTN_R3       = 1 <<  2,
	PAD_BTN_START    = 1 <<  3,
	PAD_BTN_UP       = 1 <<  4,
	PAD_BTN_RIGHT    = 1 <<  5,
	PAD_BTN_DOWN     = 1 <<  6,
	PAD_BTN_LEFT     = 1 <<  7,
	PAD_BTN_L2       = 1 <<  8,
	PAD_BTN_R2       = 1 <<  9,
	PAD_BTN_L1       = 1 << 10,
	PAD_BTN_R1       = 1 << 11,
	PAD_BTN_TRIANGLE = 1 << 12,
	PAD_BTN_CIRCLE   = 1 << 13,
	PAD_BTN_CROSS    = 1 << 14,
	PAD_BTN_SQUARE   = 1 << 15
} ControllerButton;

#ifdef __cplusplus
extern "C" {
#endif

void initControllerBus(void);
void selectControllerPort(int port);
size_t exchangeSIO0Packet(
	SIO0DeviceAddress address,
	const uint8_t     *request,
	uint8_t           *response,
	size_t            reqLength,
	size_t            maxRespLength
);

uint16_t pollController(int port);

#ifdef __cplusplus
}
#endif
