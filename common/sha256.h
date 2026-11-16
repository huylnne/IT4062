#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32

/**
 * Trạng thái tính SHA-256 dần (FIPS 180-4).
 */
typedef struct {
    uint32_t state[8];
    uint64_t total;
    uint8_t buf[64];
    size_t buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t n);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_LEN]);

/**
 * @function sha256_file: Tính SHA-256 cả file, đọc từng khối 64 KB.
 *
 * @param path: Đường dẫn file.
 * @param out: 32 byte kết quả.
 *
 * @return: 0 nếu thành công, -1 nếu không mở/đọc được file.
 */
int sha256_file(const char *path, uint8_t out[SHA256_LEN]);

/**
 * @function sha256_hex: Đổi 32 byte digest sang chuỗi hex 64 ký tự.
 *
 * @param d: Digest.
 * @param out: Bộ đệm 65 byte (kèm '\0').
 *
 * @return: void
 */
void sha256_hex(const uint8_t d[SHA256_LEN], char out[2 * SHA256_LEN + 1]);

#endif /* SHA256_H */
