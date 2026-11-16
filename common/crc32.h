#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/**
 * @function crc32_update: Cập nhật CRC32 (IEEE 802.3, đa thức 0xEDB88320)
 * với thêm n byte; dùng để tính dần nhiều đoạn.
 *
 * @param crc: Giá trị CRC trước đó (0 cho lần đầu).
 * @param buf: Dữ liệu.
 * @param n: Số byte.
 *
 * @return: CRC32 mới.
 */
uint32_t crc32_update(uint32_t crc, const void *buf, size_t n);

/**
 * @function crc32_calc: CRC32 của cả vùng nhớ (giống crc32 của zlib).
 *
 * @param buf: Dữ liệu.
 * @param n: Số byte.
 *
 * @return: CRC32.
 */
uint32_t crc32_calc(const void *buf, size_t n);

#endif /* CRC32_H */
