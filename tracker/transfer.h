#ifndef TRANSFER_H
#define TRANSFER_H

#include "peers.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

/* Số phiên truyền tối đa cùng lúc */
#define MAX_TRANSFERS 256

/**
 * Một phiên relay: src gửi chunk, dst nhận. Tracker chỉ chuyển tiếp frame,
 * không giữ dữ liệu file.
 */
typedef struct {
    uint32_t id;
    peer_t *src;
    peer_t *dst;
    char name[MAX_FILENAME + 1];
    uint64_t size;
    int64_t last_progress_ms;
} transfer_t;

/**
 * @function transfer_new: Mở phiên mới với transfer_id chưa dùng (khác 0).
 *
 * @param src: Peer nguồn.
 * @param dst: Peer tải.
 * @param name: Tên file.
 * @param size: Dung lượng.
 *
 * @return: Phiên mới, hoặc NULL nếu bảng đầy.
 */
transfer_t *transfer_new(peer_t *src, peer_t *dst, const char *name,
                         uint64_t size);

/**
 * @function transfer_find: Tìm phiên theo transfer_id.
 *
 * @param id: Mã phiên.
 *
 * @return: Phiên, hoặc NULL.
 */
transfer_t *transfer_find(uint32_t id);

/**
 * @function transfer_close: Đóng phiên (không gửi gì cho hai bên).
 *
 * @param t: Phiên.
 *
 * @return: void
 */
void transfer_close(transfer_t *t);

/**
 * @function transfer_count_src: Số phiên peer đang làm nguồn (để so với K).
 *
 * @param src: Peer.
 *
 * @return: Số phiên.
 */
int transfer_count_src(const peer_t *src);

size_t transfer_count(void);
transfer_t *transfer_at(size_t idx);

#endif /* TRANSFER_H */
