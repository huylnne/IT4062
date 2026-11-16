#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include "frame.h"
#include "peer.h"

#include <stdint.h>

/**
 * @function download_active: Đang có lượt tải chạy không (mỗi lúc một lượt).
 *
 * @return: 1 nếu có, 0 nếu không.
 */
int download_active(void);

/**
 * @function download_start: GET_OK(tid; str owner, str name, u64 size,
 * u32 nchunks, u32 chunk_size): kiểm tra rồi tạo file .part. Thông tin sai
 * thì gửi CANCEL.
 *
 * @param ctx: Ngữ cảnh.
 * @param tid: Mã phiên.
 * @param payload: Payload.
 * @param len: Độ dài payload.
 * @param owner: Chủ file đã xin trong GET.
 * @param name: Tên file đã xin trong GET.
 *
 * @return: 0 nếu bắt đầu tải, -1 nếu không.
 */
int download_start(peer_ctx_t *ctx, uint32_t tid, const uint8_t *payload,
                   uint32_t len, const char *owner, const char *name);

/**
 * @function download_owns: tid có phải lượt đang tải không.
 *
 * @param tid: Mã phiên.
 *
 * @return: 1 nếu có, 0 nếu không.
 */
int download_owns(uint32_t tid);

/**
 * @function download_on_frame: CHUNK, DONE, SOURCE_GONE, ERR của lượt tải.
 *
 * @param ctx: Ngữ cảnh.
 * @param h: Header.
 * @param payload: Payload.
 *
 * @return: void
 */
void download_on_frame(peer_ctx_t *ctx, const frame_hdr_t *h,
                       const uint8_t *payload);

/**
 * @function download_cancel: Người dùng huỷ: gửi CANCEL, xoá .part.
 *
 * @param ctx: Ngữ cảnh.
 *
 * @return: void
 */
void download_cancel(peer_ctx_t *ctx);

/**
 * @function download_tick: Gửi các ACK bị làm chậm đã tới hạn, in tiến trình.
 *
 * @param ctx: Ngữ cảnh.
 * @param now: Thời điểm hiện tại (ms).
 *
 * @return: void
 */
void download_tick(peer_ctx_t *ctx, int64_t now);

void download_status(void);

/**
 * @function download_abort_local: Xoá .part khi thoát hoặc mất kết nối,
 * không gửi gì.
 *
 * @return: void
 */
void download_abort_local(void);

/**
 * @function download_failures: Số lượt tải thất bại từ lúc chạy.
 *
 * @return: Số lượt.
 */
int download_failures(void);

#endif /* DOWNLOAD_H */
