#ifndef NETIO_H
#define NETIO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/**
 * @function now_ms: Thời gian đơn điệu (CLOCK_MONOTONIC) tính bằng ms,
 * dùng cho mọi timeout (không bị ảnh hưởng khi đổi giờ hệ thống).
 *
 * @return: Số ms.
 */
int64_t now_ms(void);

/**
 * @function set_nonblock: Bật O_NONBLOCK cho fd.
 *
 * @param fd: Descriptor.
 *
 * @return: 0 nếu thành công, -1 nếu lỗi.
 */
int set_nonblock(int fd);

/**
 * @function parse_port: Đọc số cổng 1..65535 từ chuỗi.
 *
 * @param s: Chuỗi số.
 * @param out: Cổng đọc được.
 *
 * @return: 0 nếu hợp lệ, -1 nếu không.
 */
int parse_port(const char *s, uint16_t *out);

/**
 * @function tcp_listen: Tạo socket lắng nghe 0.0.0.0:port (SO_REUSEADDR).
 *
 * @param port: Cổng.
 * @param backlog: Độ dài hàng chờ accept.
 *
 * @return: fd lắng nghe, hoặc -1 nếu lỗi (đã in perror).
 */
int tcp_listen(uint16_t port, int backlog);

/**
 * @function tcp_connect: Kết nối TCP tới host:port (blocking, thử mọi địa chỉ
 * getaddrinfo trả về).
 *
 * @param host: Tên máy hoặc IP.
 * @param port: Cổng dạng chuỗi.
 *
 * @return: fd đã kết nối, hoặc -1 nếu lỗi.
 */
int tcp_connect(const char *host, const char *port);

/**
 * @function send_all: Gửi đủ n byte trên socket blocking (MSG_NOSIGNAL).
 *
 * @param fd: Socket.
 * @param buf: Dữ liệu.
 * @param n: Số byte.
 *
 * @return: 0 nếu gửi hết, -1 nếu lỗi.
 */
int send_all(int fd, const void *buf, size_t n);

/**
 * @function recv_all: Đọc đúng n byte trên socket blocking.
 *
 * @param fd: Socket.
 * @param buf: Vùng nhớ nhận.
 * @param n: Số byte.
 *
 * @return: n nếu đủ, 0 nếu bên kia đóng, -1 nếu lỗi hoặc hết SO_RCVTIMEO.
 */
ssize_t recv_all(int fd, void *buf, size_t n);

#endif /* NETIO_H */
