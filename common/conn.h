#ifndef CONN_H
#define CONN_H

#include "frame.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

struct qblk;

/**
 * Một kết nối TCP non-blocking: bộ đệm nhận để ráp frame và hàng đợi gửi.
 * Dùng chung cho Tracker (mỗi peer một conn_t) và Peer (kết nối tới Tracker).
 */
typedef struct conn {
    int fd;
    uint8_t in[HDR_SIZE + MAX_PAYLOAD]; /* vừa đúng một frame lớn nhất */
    size_t inlen;
    int64_t partial_since_ms;  /* lúc bắt đầu frame đang dở, 0 nếu không dở */
    struct qblk *qhead;
    struct qblk *qtail;
    size_t qbytes;             /* tổng byte còn chờ gửi */
    size_t qlimit;             /* 0 = không giới hạn */
} conn_t;

/**
 * Callback nhận một frame đầy đủ. payload chỉ hợp lệ trong lúc gọi.
 * Trả 0 để đọc tiếp, khác 0 để dừng (conn_on_readable trả CONN_STOP).
 * Callback không được giải phóng c.
 */
typedef int (*frame_cb_t)(conn_t *c, const frame_hdr_t *h,
                          const uint8_t *payload, void *ctx);

/**
 * Mã trả về của các hàm conn_*.
 */
enum {
    CONN_OK = 0,
    CONN_EOF = -1,
    CONN_ERR = -2,
    CONN_BAD_FRAME = -3,
    CONN_QFULL = -4,
    CONN_STOP = -5
};

/**
 * @function conn_init: Gắn conn với fd (fd nên đã non-blocking).
 *
 * @param c: Kết nối.
 * @param fd: Socket đã kết nối.
 * @param qlimit: Giới hạn hàng đợi gửi (byte), 0 = không giới hạn.
 *
 * @return: void
 */
void conn_init(conn_t *c, int fd, size_t qlimit);

/**
 * @function conn_on_readable: Đọc dữ liệu đang có trên socket, ráp frame và
 * gọi cb cho từng frame đầy đủ. Gọi khi poll() báo POLLIN.
 *
 * @param c: Kết nối.
 * @param cb: Callback xử lý frame.
 * @param ctx: Con trỏ chuyển nguyên cho cb.
 *
 * @return: CONN_OK, CONN_EOF, CONN_ERR, CONN_BAD_FRAME (magic/version sai,
 *          length > MAX_PAYLOAD) hoặc CONN_STOP (cb yêu cầu dừng).
 */
int conn_on_readable(conn_t *c, frame_cb_t cb, void *ctx);

/**
 * @function conn_send_hdr: Xếp một frame vào hàng đợi gửi, dùng nguyên header
 * h (kể cả crc32). Dùng khi chuyển tiếp frame (relay) hoặc tiêm lỗi.
 *
 * @param c: Kết nối.
 * @param h: Header; h->length là độ dài payload.
 * @param payload: Payload (có thể NULL nếu length = 0).
 *
 * @return: CONN_OK, CONN_QFULL (vượt qlimit) hoặc CONN_ERR (hết bộ nhớ).
 */
int conn_send_hdr(conn_t *c, const frame_hdr_t *h, const void *payload);

/**
 * @function conn_send: Tạo header, tính CRC32 payload và xếp frame vào hàng đợi.
 *
 * @param c: Kết nối.
 * @param type: Loại thông điệp.
 * @param flags: Cờ (FLAG_LAST...).
 * @param transfer_id: Mã phiên.
 * @param chunk_id: Số thứ tự chunk.
 * @param payload: Payload.
 * @param len: Độ dài payload (<= MAX_PAYLOAD).
 *
 * @return: Như conn_send_hdr.
 */
int conn_send(conn_t *c, uint8_t type, uint8_t flags, uint32_t transfer_id,
              uint32_t chunk_id, const void *payload, uint32_t len);

/**
 * @function conn_send_err: Gửi ERR (u16 code, str msg).
 *
 * @param c: Kết nối.
 * @param transfer_id: Mã phiên, 0 nếu lỗi không thuộc phiên.
 * @param code: Mã lỗi (enum err_code).
 * @param msg: Mô tả ngắn.
 *
 * @return: Như conn_send_hdr.
 */
int conn_send_err(conn_t *c, uint32_t transfer_id, uint16_t code,
                  const char *msg);

/**
 * @function conn_flush: Gửi tiếp hàng đợi tới khi hết hoặc socket đầy.
 * Gọi khi poll() báo POLLOUT.
 *
 * @param c: Kết nối.
 *
 * @return: CONN_OK hoặc CONN_ERR (kết nối hỏng).
 */
int conn_flush(conn_t *c);

/**
 * @function conn_wants_write: Còn dữ liệu chờ gửi (cần đăng ký POLLOUT)?
 *
 * @param c: Kết nối.
 *
 * @return: 1 nếu còn, 0 nếu không.
 */
int conn_wants_write(const conn_t *c);

/**
 * @function conn_close: Đóng fd và giải phóng hàng đợi gửi.
 *
 * @param c: Kết nối.
 *
 * @return: void
 */
void conn_close(conn_t *c);

#endif /* CONN_H */
