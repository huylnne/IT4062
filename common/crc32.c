#include "crc32.h"

static uint32_t g_table[256];
static int g_ready;

/**
 * @function crc32_init_table: Dựng bảng tra 256 phần tử ở lần gọi đầu.
 *
 * @return: void
 */
static void crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_table[i] = c;
    }
    g_ready = 1;
}

uint32_t crc32_update(uint32_t crc, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    if (!g_ready)
        crc32_init_table();
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = g_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t crc32_calc(const void *buf, size_t n)
{
    return crc32_update(0, buf, n);
}
