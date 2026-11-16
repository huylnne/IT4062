#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "conn.h"
#include "crc32.h"
#include "netio.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Số lần recv tối đa mỗi lần gọi, để một kết nối không chiếm hết vòng poll */
#define READ_ROUNDS 16

/**
 * Một frame đã đóng gói đang chờ gửi; off là số byte đã gửi.
 */
struct qblk {
    struct qblk *next;
    size_t len;
    size_t off;
    uint8_t data[];
};

void conn_init(conn_t *c, int fd, size_t qlimit)
{
    c->fd = fd;
    c->inlen = 0;
    c->partial_since_ms = 0;
    c->qhead = NULL;
    c->qtail = NULL;
    c->qbytes = 0;
    c->qlimit = qlimit;
}

/**
 * @function parse_frames: Tách các frame đầy đủ trong c->in, gọi cb, dồn phần
 * dư về đầu bộ đệm và cập nhật partial_since_ms.
 *
 * @param c: Kết nối.
 * @param cb: Callback.
 * @param ctx: Ngữ cảnh cho cb.
 *
 * @return: CONN_OK, CONN_BAD_FRAME hoặc CONN_STOP.
 */
static int parse_frames(conn_t *c, frame_cb_t cb, void *ctx)
{
    size_t pos = 0;
    int got = 0;
    int rc = CONN_OK;

    while (c->inlen - pos >= HDR_SIZE) {
        frame_hdr_t h;
        if (hdr_unpack(c->in + pos, &h) != FRAME_OK) {
            rc = CONN_BAD_FRAME;
            break;
        }
        size_t total = HDR_SIZE + h.length;
        if (c->inlen - pos < total)
            break;

        const uint8_t *payload = c->in + pos + HDR_SIZE;
        pos += total;
        got = 1;
        if (cb(c, &h, payload, ctx) != 0) {
            rc = CONN_STOP;
            break;
        }
    }

    if (pos > 0) {
        memmove(c->in, c->in + pos, c->inlen - pos);
        c->inlen -= pos;
    }

    /* Đồng hồ frame dở tính lại từ lúc frame gần nhất hoàn tất */
    if (c->inlen == 0)
        c->partial_since_ms = 0;
    else if (got || c->partial_since_ms == 0)
        c->partial_since_ms = now_ms();
    return rc;
}

int conn_on_readable(conn_t *c, frame_cb_t cb, void *ctx)
{
    for (int round = 0; round < READ_ROUNDS; round++) {
        size_t room = sizeof(c->in) - c->inlen;
        if (room == 0)
            return CONN_BAD_FRAME;

        ssize_t k = recv(c->fd, c->in + c->inlen, room, 0);
        if (k == 0)
            return CONN_EOF;
        if (k < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return CONN_OK;
            return CONN_ERR;
        }
        c->inlen += (size_t)k;

        int rc = parse_frames(c, cb, ctx);
        if (rc != CONN_OK)
            return rc;
    }
    return CONN_OK;
}

int conn_send_hdr(conn_t *c, const frame_hdr_t *h, const void *payload)
{
    if (h->length > MAX_PAYLOAD)
        return CONN_ERR;

    size_t total = HDR_SIZE + h->length;
    if (c->qlimit && c->qbytes + total > c->qlimit)
        return CONN_QFULL;

    struct qblk *b = malloc(sizeof(*b) + total);
    if (!b)
        return CONN_ERR;
    b->next = NULL;
    b->len = total;
    b->off = 0;
    hdr_pack(h, b->data);
    if (h->length > 0)
        memcpy(b->data + HDR_SIZE, payload, h->length);

    if (c->qtail)
        c->qtail->next = b;
    else
        c->qhead = b;
    c->qtail = b;
    c->qbytes += total;
    return CONN_OK;
}

int conn_send(conn_t *c, uint8_t type, uint8_t flags, uint32_t transfer_id,
              uint32_t chunk_id, const void *payload, uint32_t len)
{
    frame_hdr_t h;
    hdr_init(&h, type, transfer_id, chunk_id);
    h.flags = flags;
    h.length = len;
    h.crc32 = len > 0 ? crc32_calc(payload, len) : 0;
    return conn_send_hdr(c, &h, payload);
}

int conn_send_err(conn_t *c, uint32_t transfer_id, uint16_t code,
                  const char *msg)
{
    uint8_t buf[2 + 1 + 255];
    wbuf_t w;
    wbuf_init(&w, buf, sizeof(buf));
    put_u16(&w, code);
    put_str(&w, msg);
    if (w.err)
        return CONN_ERR;
    return conn_send(c, MSG_ERR, 0, transfer_id, 0, buf, (uint32_t)w.len);
}

int conn_flush(conn_t *c)
{
    while (c->qhead) {
        struct qblk *b = c->qhead;
        ssize_t k = send(c->fd, b->data + b->off, b->len - b->off,
                         MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return CONN_OK;
            return CONN_ERR;
        }
        b->off += (size_t)k;
        c->qbytes -= (size_t)k;
        if (b->off == b->len) {
            c->qhead = b->next;
            if (!c->qhead)
                c->qtail = NULL;
            free(b);
        }
    }
    return CONN_OK;
}

int conn_wants_write(const conn_t *c)
{
    return c->qhead != NULL;
}

void conn_close(conn_t *c)
{
    while (c->qhead) {
        struct qblk *b = c->qhead;
        c->qhead = b->next;
        free(b);
    }
    c->qtail = NULL;
    c->qbytes = 0;
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}
