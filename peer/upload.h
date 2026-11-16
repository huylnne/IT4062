#ifndef UPLOAD_H
#define UPLOAD_H

#include "frame.h"
#include "peer.h"

#include <stdint.h>

/* Số phiên gửi tối đa phía peer (Tracker đã giới hạn K, đây là chốt chặn) */
#define MAX_UPLOADS 4

/**
 * @function upload_on_send_req: SEND_REQ(tid; str requester, str name,
 * u64 size): mở file và bắt đầu gửi. Lỗi thì trả ERR(tid) cho Tracker.
 *
 * @param ctx: Ngữ cảnh.
 * @param tid: Mã phiên.
 * @param payload: Payload.
 * @param len: Độ dài payload.
 *
 * @return: void
 */
void upload_on_send_req(peer_ctx_t *ctx, uint32_t tid, const uint8_t *payload,
                        uint32_t len);

/**
 * @function upload_owns: tid có phải phiên mình đang gửi không.
 *
 * @param tid: Mã phiên.
 *
 * @return: 1 nếu có, 0 nếu không.
 */
int upload_owns(uint32_t tid);

/**
 * @function upload_on_frame: ACK, NACK, REQ_RANGE, ABORT, OK, ERR của phiên gửi.
 *
 * @param ctx: Ngữ cảnh.
 * @param h: Header.
 * @param payload: Payload.
 *
 * @return: void
 */
void upload_on_frame(peer_ctx_t *ctx, const frame_hdr_t *h,
                     const uint8_t *payload);

/**
 * @function upload_tick: Gửi thêm chunk khi cửa sổ còn chỗ, gửi lại chunk
 * bị NACK/REQ_RANGE hoặc quá ACK_TIMEOUT_MS chưa có ACK, gửi DONE.
 *
 * @param ctx: Ngữ cảnh.
 * @param now: Thời điểm hiện tại (ms).
 *
 * @return: void
 */
void upload_tick(peer_ctx_t *ctx, int64_t now);

/**
 * @function upload_count: Số phiên đang gửi.
 *
 * @return: Số phiên.
 */
int upload_count(void);

void upload_status(void);

/**
 * @function upload_close_all: Đóng mọi phiên gửi (khi thoát), không báo ai.
 *
 * @return: void
 */
void upload_close_all(void);

#endif /* UPLOAD_H */
