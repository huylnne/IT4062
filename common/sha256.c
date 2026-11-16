#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/**
 * @function sha256_block: Xử lý một khối 64 byte.
 *
 * @param s: 8 từ trạng thái.
 * @param b: Khối 64 byte.
 *
 * @return: void
 */
static void sha256_block(uint32_t s[8], const uint8_t b[64])
{
    uint32_t w[64];

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[4 * i] << 24) | ((uint32_t)b[4 * i + 1] << 16) |
               ((uint32_t)b[4 * i + 2] << 8) | (uint32_t)b[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = s[0], bb = s[1], c = s[2], d = s[3];
    uint32_t e = s[4], f = s[5], g = s[6], h = s[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & bb) ^ (a & c) ^ (bb & c);
        uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = bb;
        bb = a;
        a = t1 + t2;
    }

    s[0] += a;
    s[1] += bb;
    s[2] += c;
    s[3] += d;
    s[4] += e;
    s[5] += f;
    s[6] += g;
    s[7] += h;
}

void sha256_init(sha256_ctx_t *ctx)
{
    static const uint32_t init[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    memcpy(ctx->state, init, sizeof(init));
    ctx->total = 0;
    ctx->buflen = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, size_t n)
{
    const uint8_t *p = data;

    ctx->total += n;
    if (ctx->buflen > 0) {
        size_t take = 64 - ctx->buflen;
        if (take > n)
            take = n;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        n -= take;
        if (ctx->buflen < 64)
            return;
        sha256_block(ctx->state, ctx->buf);
        ctx->buflen = 0;
    }
    while (n >= 64) {
        sha256_block(ctx->state, p);
        p += 64;
        n -= 64;
    }
    memcpy(ctx->buf, p, n);
    ctx->buflen = n;
}

void sha256_final(sha256_ctx_t *ctx, uint8_t out[SHA256_LEN])
{
    uint64_t bits = ctx->total * 8;

    /* Đệm: 0x80, các byte 0, rồi 8 byte độ dài bit (big-endian) */
    ctx->buf[ctx->buflen++] = 0x80;
    if (ctx->buflen > 56) {
        memset(ctx->buf + ctx->buflen, 0, 64 - ctx->buflen);
        sha256_block(ctx->state, ctx->buf);
        ctx->buflen = 0;
    }
    memset(ctx->buf + ctx->buflen, 0, 56 - ctx->buflen);
    for (int i = 0; i < 8; i++)
        ctx->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_block(ctx->state, ctx->buf);

    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(ctx->state[i] >> 24);
        out[4 * i + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[4 * i + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[4 * i + 3] = (uint8_t)ctx->state[i];
    }
}

int sha256_file(const char *path, uint8_t out[SHA256_LEN])
{
    static uint8_t buf[64 * 1024];
    sha256_ctx_t ctx;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;
    sha256_init(&ctx);
    for (;;) {
        ssize_t k = read(fd, buf, sizeof(buf));
        if (k < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (k == 0)
            break;
        sha256_update(&ctx, buf, (size_t)k);
    }
    close(fd);
    sha256_final(&ctx, out);
    return 0;
}

void sha256_hex(const uint8_t d[SHA256_LEN], char out[2 * SHA256_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < SHA256_LEN; i++) {
        out[2 * i] = hex[d[i] >> 4];
        out[2 * i + 1] = hex[d[i] & 0xF];
    }
    out[2 * SHA256_LEN] = '\0';
}
