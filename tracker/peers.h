#ifndef PEERS_H
#define PEERS_H

#include "conn.h"
#include "protocol.h"

#include <stddef.h>

/* Số kết nối tối đa Tracker phục vụ cùng lúc */
#define MAX_PEERS 128

/**
 * Trạng thái đóng của một peer (dọn ở cuối mỗi vòng poll).
 */
enum {
    CLOSE_NONE = 0,
    CLOSE_FLUSH = 1, /* gửi nốt hàng đợi (ERR, OK của QUIT) rồi đóng */
    CLOSE_NOW = 2    /* kết nối đã hỏng, đóng ngay */
};

/**
 * Một kết nối tới Tracker; username rỗng nghĩa là chưa JOIN.
 */
typedef struct peer {
    conn_t conn;
    char username[MAX_USERNAME + 1];
    char addr[64];
    int closing;
} peer_t;

/**
 * @function peers_add: Cấp phát peer mới cho fd vừa accept.
 *
 * @param fd: Socket đã non-blocking.
 * @param addr: Chuỗi "ip:port" để ghi log.
 *
 * @return: Peer mới, hoặc NULL nếu đầy hoặc hết bộ nhớ.
 */
peer_t *peers_add(int fd, const char *addr);

/**
 * @function peers_remove: Đóng kết nối, giải phóng peer và bỏ khỏi bảng.
 * Các peer sau nó dồn lên một chỉ số.
 *
 * @param idx: Chỉ số trong bảng.
 *
 * @return: void
 */
void peers_remove(size_t idx);

/**
 * @function peers_find_user: Tìm peer đang online theo username.
 *
 * @param name: Username.
 *
 * @return: Peer, hoặc NULL nếu không có.
 */
peer_t *peers_find_user(const char *name);

size_t peers_count(void);
peer_t *peers_at(size_t idx);

#endif /* PEERS_H */
