#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "frame.h"

#include <string.h>

/*
 * Ghi/đọc số nhiều byte bằng phép dịch bit: kết quả giống htons/htonl
 * (big-endian) nhưng không phụ thuộc căn lề bộ nhớ của vùng đệm.
 */
static void store_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t load_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void hdr_init(frame_hdr_t *h, uint8_t type, uint32_t transfer_id,
              uint32_t chunk_id)
{
    memset(h, 0, sizeof(*h));
    h->magic = PROTO_MAGIC;
    h->version = PROTO_VERSION;
    h->type = type;
    h->transfer_id = transfer_id;
    h->chunk_id = chunk_id;
}

void hdr_pack(const frame_hdr_t *h, uint8_t out[HDR_SIZE])
{
    store_be16(out + 0, h->magic);
    out[2] = h->version;
    out[3] = h->type;
    out[4] = h->flags;
    out[5] = h->reserved;
    store_be32(out + 6, h->transfer_id);
    store_be32(out + 10, h->chunk_id);
    store_be32(out + 14, h->length);
    store_be32(out + 18, h->crc32);
}

int hdr_unpack(const uint8_t in[HDR_SIZE], frame_hdr_t *h)
{
    h->magic = load_be16(in + 0);
    h->version = in[2];
    h->type = in[3];
    h->flags = in[4];
    h->reserved = in[5];
    h->transfer_id = load_be32(in + 6);
    h->chunk_id = load_be32(in + 10);
    h->length = load_be32(in + 14);
    h->crc32 = load_be32(in + 18);

    if (h->magic != PROTO_MAGIC)
        return FRAME_BAD_MAGIC;
    if (h->version != PROTO_VERSION)
        return FRAME_BAD_VERSION;
    if (h->length > MAX_PAYLOAD)
        return FRAME_TOO_LONG;
    return FRAME_OK;
}

void wbuf_init(wbuf_t *w, void *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = 0;
}

/**
 * @function wbuf_reserve: Lấy chỗ cho n byte tiếp theo.
 *
 * @param w: Bộ ghi.
 * @param n: Số byte cần.
 *
 * @return: Con trỏ tới chỗ ghi, hoặc NULL (và đặt err) nếu tràn.
 */
static uint8_t *wbuf_reserve(wbuf_t *w, size_t n)
{
    if (w->err || n > w->cap - w->len) {
        w->err = 1;
        return NULL;
    }
    uint8_t *p = w->buf + w->len;
    w->len += n;
    return p;
}

void put_u8(wbuf_t *w, uint8_t v)
{
    uint8_t *p = wbuf_reserve(w, 1);
    if (p)
        p[0] = v;
}

void put_u16(wbuf_t *w, uint16_t v)
{
    uint8_t *p = wbuf_reserve(w, 2);
    if (p)
        store_be16(p, v);
}

void put_u32(wbuf_t *w, uint32_t v)
{
    uint8_t *p = wbuf_reserve(w, 4);
    if (p)
        store_be32(p, v);
}

void put_u64(wbuf_t *w, uint64_t v)
{
    uint8_t *p = wbuf_reserve(w, 8);
    if (p) {
        store_be32(p, (uint32_t)(v >> 32));
        store_be32(p + 4, (uint32_t)v);
    }
}

void put_bytes(wbuf_t *w, const void *src, size_t n)
{
    uint8_t *p = wbuf_reserve(w, n);
    if (p && n > 0)
        memcpy(p, src, n);
}

void put_str(wbuf_t *w, const char *s)
{
    size_t n = strlen(s);
    if (n > 255) {
        w->err = 1;
        return;
    }
    put_u8(w, (uint8_t)n);
    put_bytes(w, s, n);
}

void rbuf_init(rbuf_t *r, const void *p, size_t len)
{
    r->p = p;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

/**
 * @function rbuf_take: Lấy n byte tiếp theo của payload.
 *
 * @param r: Bộ đọc.
 * @param n: Số byte cần.
 *
 * @return: Con trỏ tới n byte, hoặc NULL (và đặt err) nếu thiếu.
 */
static const uint8_t *rbuf_take(rbuf_t *r, size_t n)
{
    if (r->err || n > r->len - r->pos) {
        r->err = 1;
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    return p;
}

uint8_t get_u8(rbuf_t *r)
{
    const uint8_t *p = rbuf_take(r, 1);
    return p ? p[0] : 0;
}

uint16_t get_u16(rbuf_t *r)
{
    const uint8_t *p = rbuf_take(r, 2);
    return p ? load_be16(p) : 0;
}

uint32_t get_u32(rbuf_t *r)
{
    const uint8_t *p = rbuf_take(r, 4);
    return p ? load_be32(p) : 0;
}

uint64_t get_u64(rbuf_t *r)
{
    const uint8_t *p = rbuf_take(r, 8);
    if (!p)
        return 0;
    return ((uint64_t)load_be32(p) << 32) | load_be32(p + 4);
}

int get_bytes(rbuf_t *r, void *out, size_t n)
{
    const uint8_t *p = rbuf_take(r, n);
    if (!p)
        return -1;
    if (n > 0)
        memcpy(out, p, n);
    return 0;
}

int get_str(rbuf_t *r, char *out, size_t cap)
{
    size_t n = get_u8(r);
    const uint8_t *p = rbuf_take(r, n);
    if (!p || n >= cap || memchr(p, '\0', n)) {
        r->err = 1;
        return -1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return (int)n;
}

int rbuf_done(const rbuf_t *r)
{
    return !r->err && r->pos == r->len;
}
