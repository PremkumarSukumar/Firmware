/****************************************************************************
 *
 *   Copyright (C) 2017 PX4 Development Team. All rights reserved.
 *   Author: @author David Sidrane <david_s5@nscdg.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file board_identity.c
 * Implementation of STM32 based Board identity API
 */

#include <px4_config.h>
#include <stdio.h>
#include <string.h>

/* A type suitable for holding the reordering array for the byte format of the UUID
 */
typedef const uint8_t uuid_uint8_reorder_t[PX4_CPU_UUID_BYTE_LENGTH];
typedef const uint8_t raw_uuid_uint32_reorder_t[PX4_CPU_UUID_WORD32_LENGTH];

#define SWAP_UINT32(x) (((x) >> 24) | (((x) & 0x00ff0000) >> 8) | (((x) & 0x0000ff00) << 8) | ((x) << 24))

void board_get_uuid_raw(raw_uuid_byte_t *raw_uuid)
{
	memcpy(raw_uuid, (uint8_t *) STM32_SYSMEM_UID, PX4_CPU_UUID_BYTE_LENGTH);
}

void board_get_uuid(raw_uuid_byte_t uuid)
{
	uuid_uint8_reorder_t reorder = PX4_CPU_UUID_BYTE_FORMAT_ORDER;
	raw_uuid_byte_t raw_uuid;

	/* Copy the serial from the chips non-write memory */

	board_get_uuid_raw(&raw_uuid);

	/* swap endianess */

	for (int i = 0; i < PX4_CPU_UUID_BYTE_LENGTH; i++) {
		uuid[i] = raw_uuid[reorder[i]];
	}
}

__EXPORT void board_get_uuid32(raw_uuid_uint32_t raw_uuid_words)
{
	uint32_t *chip_uuid = (uint32_t *) STM32_SYSMEM_UID;

	for (int i = 0; i < PX4_CPU_UUID_WORD32_LENGTH; i++) {
		raw_uuid_words[i] = chip_uuid[i];
	}
}

int board_get_uuid_formated32(char *format_buffer, int size,
			      const char *format,
			      const char *seperator)
{
	raw_uuid_uint32_t uuid;
	board_get_uuid32(uuid);
	int offset = 0;
	int sep_size = seperator ? strlen(seperator) : 0;

	for (int i = 0; i < PX4_CPU_UUID_WORD32_LENGTH; i++) {
		offset += snprintf(&format_buffer[offset], size - ((i * 2 * sizeof(uint32_t)) + 1), format, uuid[i]);

		if (sep_size && i < PX4_CPU_UUID_WORD32_LENGTH - 1) {
			strcat(&format_buffer[offset], seperator);
			offset += sep_size;
		}
	}

	return 0;
}

int board_get_mfguid(mfguid_t mfgid)
{
	uint32_t *chip_uuid = (uint32_t *) STM32_SYSMEM_UID;
	uint32_t  *rv = (uint32_t *) &mfgid[0];

	for (int i = 0; i < PX4_CPU_UUID_WORD32_LENGTH; i++) {
		*rv++ = SWAP_UINT32(chip_uuid[(PX4_CPU_UUID_WORD32_LENGTH - 1) - i]);
	}

	return PX4_CPU_MFGUID_BYTE_LENGTH;
}

int board_get_mfguid_formated(char *format_buffer, int size)
{
	mfguid_t mfguid;

	board_get_mfguid(mfguid);
	int offset  = 0;

	for (unsigned int i = 0; i < PX4_CPU_MFGUID_BYTE_LENGTH; i++) {
		offset += snprintf(&format_buffer[offset], size - offset, "%02x" , mfguid[i]);
	}

	return offset;
}
