#ifndef HANDLERS_H
#define HANDLERS_H

#include "conn.h"
#include "frame.h"
#include "peers.h"

/**
 * @function handle_frame: Xử lý một frame từ peer (callback cho
 * conn_on_readable, ctx là peer_t*).
 *
 * @param c: Kết nối của peer.
 * @param h: Header.
 * @param payload: Payload (h->length byte).
 * @param ctx: peer_t* gửi frame.
 *
 * @return: 0 để đọc tiếp, 1 nếu peer đã bị đánh dấu đóng.
 */
int handle_frame(conn_t *c, const frame_hdr_t *h, const uint8_t *payload,
                 void *ctx);

/**
 * @function handle_disconnect: Dọn mọi thứ gắn với peer trước khi xoá nó
 * (catalog, các phiên truyền đang mở).
 *
 * @param p: Peer sắp bị xoá.
 *
 * @return: void
 */
void handle_disconnect(peer_t *p);

/**
 * @function handlers_tick: Huỷ các phiên không tiến triển quá NO_PROGRESS_MS
 * (ABORT cho nguồn, ERR NO_PROGRESS cho người tải).
 *
 * @param now: Thời điểm hiện tại (ms).
 *
 * @return: void
 */
void handlers_tick(int64_t now);

#endif /* HANDLERS_H */
