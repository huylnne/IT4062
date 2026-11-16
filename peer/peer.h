#ifndef PEER_H
#define PEER_H

#include "conn.h"
#include "fault.h"

#include <stddef.h>
#include <stdint.h>

/**
 * Ngữ cảnh dùng chung giữa các module của Peer.
 */
typedef struct {
    conn_t conn;            /* kết nối tới Tracker */
    const char *username;
    const char *dl_dir;     /* thư mục lưu file tải về */
    int interactive;        /* stdin là terminal: in tiến trình bằng \r */
    fault_t fault;          /* tuỳ chọn tiêm lỗi */
} peer_ctx_t;

/**
 * @function peer_send: Xếp frame lên kết nối Tracker; lỗi thì đánh dấu kết
 * nối hỏng để vòng lặp chính thoát.
 *
 * @param ctx: Ngữ cảnh.
 * @param type: Loại thông điệp.
 * @param tid: Mã phiên.
 * @param cid: Số thứ tự chunk.
 * @param payload: Payload.
 * @param len: Độ dài payload.
 *
 * @return: 0 nếu thành công, -1 nếu lỗi.
 */
int peer_send(peer_ctx_t *ctx, uint8_t type, uint32_t tid, uint32_t cid,
              const void *payload, uint32_t len);

/**
 * @function human_size: Định dạng số byte, ví dụ "12.3 MB".
 *
 * @param n: Số byte.
 * @param out: Bộ đệm.
 * @param cap: Dung lượng bộ đệm.
 *
 * @return: out.
 */
char *human_size(uint64_t n, char *out, size_t cap);

#endif /* PEER_H */
