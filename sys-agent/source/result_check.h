#pragma once

/* Result-code classification shared by the runtime and the host tests.
 * Kept free of libnx headers so tests/ can compile it off-target.
 *
 * libnx/libvapours decode a 32-bit Result as
 *   module      = value & 0x1FF         (bits 0-8)
 *   description = (value >> 9) & 0x1FFF (bits 9-21)
 * so sm:OutOfSessions (module 21, description 3) encodes as 0x615, which fatal
 * reports show as "2021-0003". */

#include <stdbool.h>
#include <stdint.h>

#define SYS_AGENT_SM_MODULE 21u
#define SYS_AGENT_SM_OUT_OF_SESSIONS 3u

bool resultIsOutOfSessions(uint32_t rc);
