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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "common/sio0.h"
#include "ps1/registers.h"

static void delayMicroseconds(int time) {
	time = ((time * 271) + 4) / 8;

	__asm__ volatile(
		".set push\n"
		".set noreorder\n"
		"bgtz  %0, .\n"
		"addiu %0, -2\n"
		".set pop\n"
		: "+r"(time)
	);
}

void initControllerBus(void) {
	SIO_CTRL(0) = SIO_CTRL_RESET;

	SIO_MODE(0) = 0
		| SIO_MODE_BAUD_DIV1
		| SIO_MODE_DATA_8;
	SIO_BAUD(0) = F_CPU / 250000;
	SIO_CTRL(0) = 0
		| SIO_CTRL_TX_ENABLE
		| SIO_CTRL_RX_ENABLE
		| SIO_CTRL_DSR_IRQ_ENABLE;
}

static bool waitForAcknowledge(int timeout) {
	for (; timeout > 0; timeout -= 10) {
		if (IRQ_STAT & (1 << IRQ_SIO0)) {
			IRQ_STAT     = ~(1 << IRQ_SIO0);
			SIO_CTRL(0) |= SIO_CTRL_ACKNOWLEDGE;

			return true;
		}

		delayMicroseconds(10);
	}

	return false;
}

#define DTR_DELAY    60
#define DSR_TIMEOUT 120

void selectControllerPort(int port) {
	if (port)
		SIO_CTRL(0) |= SIO_CTRL_CS_PORT_2;
	else
		SIO_CTRL(0) &= ~SIO_CTRL_CS_PORT_2;
}

static uint8_t exchangeByte(uint8_t value) {
	while (!(SIO_STAT(0) & SIO_STAT_TX_NOT_FULL))
		__asm__ volatile("");

	SIO_DATA(0) = value;

	while (!(SIO_STAT(0) & SIO_STAT_RX_NOT_EMPTY))
		__asm__ volatile("");

	return SIO_DATA(0);
}

size_t exchangeSIO0Packet(
	SIO0DeviceAddress address,
	const uint8_t     *request,
	uint8_t           *response,
	size_t            reqLength,
	size_t            maxRespLength
) {
	IRQ_STAT     = ~(1 << IRQ_SIO0);
	SIO_CTRL(0) |= SIO_CTRL_DTR | SIO_CTRL_ACKNOWLEDGE;
	delayMicroseconds(DTR_DELAY);

	size_t respLength = 0;

	SIO_DATA(0) = address;

	if (waitForAcknowledge(DSR_TIMEOUT)) {
		while (SIO_STAT(0) & SIO_STAT_RX_NOT_EMPTY)
			SIO_DATA(0);

		while (respLength < maxRespLength) {
			if (reqLength > 0) {
				*(response++) = exchangeByte(*(request++));
				reqLength--;
			} else {
				*(response++) = exchangeByte(0);
			}

			respLength++;

			// FIXME: this is not 100% reliable due to metastability issues...
			if (!waitForAcknowledge(DSR_TIMEOUT))
				break;
		}
	}

	delayMicroseconds(DTR_DELAY);
	SIO_CTRL(0) &= ~SIO_CTRL_DTR;

	return respLength;
}

uint16_t pollController(int port) {
	uint8_t request[4], response[8];

	request[0] = SIO0_PAD_POLL; // Command
	request[1] = 0x00;          // Multitap address
	request[2] = 0x00;          // Actuator control 1
	request[3] = 0x00;          // Actuator control 2

	selectControllerPort(port);
	size_t respLength = exchangeSIO0Packet(
		SIO0_ADDR_CONTROLLER,
		request,
		response,
		sizeof(request),
		sizeof(response)
	);

	if (respLength < 4)
		return 0;
	if ((response[0] >> 4) == PAD_TYPE_NONE)
		return 0;

	return (response[2] | (response[3] << 8)) ^ 0xffff;
}
