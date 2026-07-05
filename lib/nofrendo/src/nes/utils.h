#pragma once

/*
 * Platform shim for the retro-go nofrendo fork on Arduino/ESP32 (UNIHIKER K10).
 *
 * The upstream file pulled in retro-go's <rg_system.h> for logging and CRC32.
 * Here we provide plain-C equivalents so the nofrendo core compiles standalone
 * under the Arduino framework. Nothing else in the core references retro-go.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* level is ignored; messages go to stdout (USB CDC / UART0). */
#define LOG_PRINTF(level, ...) printf(__VA_ARGS__)

/* ESP-IDF defines IRAM_ATTR (via Arduino.h); provide a no-op fallback for the
 * plain-C translation units in the core that do not include it. */
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

/* CRC32 (IEEE 802.3) used by the ROM database / save-state naming.
 * static inline => one copy per translation unit, no link-time collisions. */
static inline uint32_t nofrendo_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}
/* Callers use CRC32(seed, data, len); we ignore the seed and hash (data, len). */
#define CRC32(a, b, c) nofrendo_crc32((b), (c))

#define MESSAGE_ERROR(...) LOG_PRINTF(1, "!! " __VA_ARGS__)
#define MESSAGE_WARN(...)  LOG_PRINTF(2, " ! " __VA_ARGS__)
#define MESSAGE_INFO(...)  LOG_PRINTF(3, __VA_ARGS__)
#ifdef NOFRENDO_DEBUG
#define MESSAGE_DEBUG(fmt, ...) LOG_PRINTF(4, "> %s: " fmt, __func__, ##__VA_ARGS__)
#else
#define MESSAGE_DEBUG(fmt, ...)
#endif
#define MESSAGE_TRACE(fmt, ...) LOG_PRINTF(4, "~ %s: " fmt, __func__, ##__VA_ARGS__)

#undef MIN
#define MIN(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#undef MAX
#define MAX(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })

#define ASSERT(expr) do { if (!(expr)) { LOG_PRINTF(1, "ASSERTION FAILED IN %s: " #expr "\n", __func__); abort(); } } while (0)
#define UNUSED(x) (void)(x)
