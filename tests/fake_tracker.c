#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

/*
 * fake_tracker: Tracker giả, phục vụ lần lượt từng peer, để test Peer mà
 * không cần Tracker thật.
 *
 *   ./bin/fake_tracker <port> [--reject ERR_NAME] [--once]
 *
 * JOIN -> OK (hoặc ERR ERR_NAME nếu có --reject), QUIT -> OK rồi đóng,
 * mọi frame khác được in ra và trả ERR UNKNOWN_TYPE.
 * Các tuần sau thêm chế độ gửi SEND_REQ / GET_OK + chunk để test truyền file.
 */

#include "frame.h"
#include "netio.h"
#include "protocol.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "fake_tracker"

static int send_simple(int fd, uint8_t type, const void *payload,
                       uint32_t len)
{
    uint8_t hdr[HDR_SIZE];
    frame_hdr_t h;
    hdr_init(&h, type, 0, 0);
    h.length = len;
    hdr_pack(&h, hdr);
    if (send_all(fd, hdr, HDR_SIZE) < 0)
        return -1;
    return len > 0 ? send_all(fd, payload, len) : 0;
}

static int send_err(int fd, uint16_t code, const char *msg)
{
    uint8_t buf[300];
    wbuf_t w;
    wbuf_init(&w, buf, sizeof(buf));
    put_u16(&w, code);
    put_str(&w, msg);
    return send_simple(fd, MSG_ERR, buf, (uint32_t)w.len);
}

/**
 * @function serve: Phục vụ một peer tới khi nó QUIT hoặc đóng kết nối.
 *
 * @param fd: Socket đã accept.
 * @param reject: Mã lỗi trả cho JOIN, 0 nếu chấp nhận.
 *
 * @return: void
 */
static void serve(int fd, int reject)
{
    static uint8_t payload[MAX_PAYLOAD];

    for (;;) {
        uint8_t hdr[HDR_SIZE];
        frame_hdr_t h;

        if (recv_all(fd, hdr, HDR_SIZE) <= 0)
            break;
        if (hdr_unpack(hdr, &h) != FRAME_OK) {
            log_msg(TAG, "header hỏng");
            send_err(fd, E_BAD_FRAME, "bad frame");
            break;
        }
        if (h.length > 0 && recv_all(fd, payload, h.length) <= 0)
            break;

        if (h.type == MSG_JOIN) {
            char name[MAX_USERNAME + 1] = "";
            rbuf_t r;
            rbuf_init(&r, payload, h.length);
            get_str(&r, name, sizeof(name));
            log_msg(TAG, "JOIN %s", name);
            if (reject)
                send_err(fd, (uint16_t)reject, "từ chối (fake)");
            else
                send_simple(fd, MSG_OK, NULL, 0);
        } else if (h.type == MSG_QUIT) {
            log_msg(TAG, "QUIT");
            send_simple(fd, MSG_OK, NULL, 0);
            break;
        } else {
            log_msg(TAG, "nhận %s len=%u", msg_type_name(h.type), h.length);
            send_err(fd, E_UNKNOWN_TYPE, "fake_tracker chưa hỗ trợ");
        }
    }
    close(fd);
    log_msg(TAG, "peer ngắt kết nối");
}

int main(int argc, char **argv)
{
    uint16_t port;
    int reject = 0;
    int once = 0;

    if (argc < 2 || parse_port(argv[1], &port) < 0) {
        fprintf(stderr, "Usage: %s <port> [--reject ERR_NAME] [--once]\n",
                argv[0]);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--reject") == 0 && i + 1 < argc) {
            reject = err_code_from_name(argv[++i]);
            if (reject < 0) {
                fprintf(stderr, "Mã lỗi không hợp lệ: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else {
            fprintf(stderr, "Tuỳ chọn không hợp lệ: %s\n", argv[i]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    int lfd = tcp_listen(port, 8);
    if (lfd < 0)
        return 1;
    log_msg(TAG, "lắng nghe cổng %u", port);

    do {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            perror("accept");
            continue;
        }
        serve(fd, reject);
    } while (!once);

    close(lfd);
    return 0;
}
