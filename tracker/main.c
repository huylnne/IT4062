#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "conn.h"
#include "handlers.h"
#include "netio.h"
#include "peers.h"
#include "protocol.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "tracker"

/* Chu kỳ tối đa giữa hai lần kiểm tra timeout (ms) */
#define POLL_TICK_MS 500

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/**
 * @function accept_all: Nhận mọi kết nối đang chờ trên socket lắng nghe.
 *
 * @param lfd: Socket lắng nghe (non-blocking).
 *
 * @return: void
 */
static void accept_all(int lfd)
{
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int fd = accept(lfd, (struct sockaddr *)&sa, &sl);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept");
            return;
        }

        char addr[64];
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
        snprintf(addr, sizeof(addr), "%s:%u", ip, ntohs(sa.sin_port));

        if (set_nonblock(fd) < 0 || !peers_add(fd, addr)) {
            log_msg(TAG, "%s: từ chối (đầy hoặc lỗi)", addr);
            close(fd);
            continue;
        }
        log_msg(TAG, "%s kết nối", addr);
    }
}

/**
 * @function on_events: Xử lý sự kiện poll của một peer.
 *
 * @param p: Peer.
 * @param re: revents.
 *
 * @return: void
 */
static void on_events(peer_t *p, short re)
{
    if (re & POLLNVAL) {
        p->closing = CLOSE_NOW;
        return;
    }
    if (re & (POLLIN | POLLHUP | POLLERR)) {
        int rc = conn_on_readable(&p->conn, handle_frame, p);
        if (rc == CONN_BAD_FRAME) {
            log_msg(TAG, "%s: frame hỏng, đóng kết nối", p->addr);
            conn_send_err(&p->conn, 0, E_BAD_FRAME,
                          "magic/version sai hoặc length quá lớn");
            p->closing = CLOSE_FLUSH;
            return;
        }
        if (rc == CONN_EOF || rc == CONN_ERR) {
            p->closing = CLOSE_NOW;
            return;
        }
    }
    if (!p->closing && (re & POLLOUT) && conn_flush(&p->conn) != CONN_OK)
        p->closing = CLOSE_NOW;
}

/**
 * @function check_timeouts: Đóng các peer gửi dở một frame quá lâu.
 *
 * @param now: Thời điểm hiện tại (ms).
 *
 * @return: void
 */
static void check_timeouts(int64_t now)
{
    for (size_t i = 0; i < peers_count(); i++) {
        peer_t *p = peers_at(i);
        if (p->closing || p->conn.partial_since_ms == 0)
            continue;
        if (now - p->conn.partial_since_ms > PARTIAL_FRAME_MS) {
            log_msg(TAG, "%s: frame dở quá %d ms, đóng kết nối", p->addr,
                    PARTIAL_FRAME_MS);
            conn_send_err(&p->conn, 0, E_TIMEOUT, "frame đọc dở quá lâu");
            p->closing = CLOSE_FLUSH;
        }
    }
}

/**
 * @function reap_closed: Dọn các peer đã đánh dấu đóng. CLOSE_FLUSH được gửi
 * nốt hàng đợi một lần (best effort) trước khi đóng.
 *
 * @return: void
 */
static void reap_closed(void)
{
    size_t i = 0;
    while (i < peers_count()) {
        peer_t *p = peers_at(i);
        if (!p->closing) {
            i++;
            continue;
        }
        if (p->closing == CLOSE_FLUSH) {
            conn_flush(&p->conn);
            /* Đóng khi còn dữ liệu chưa đọc sẽ sinh RST, có thể làm mất ERR
               vừa gửi: báo FIN rồi xả phần đang chờ trong bộ đệm nhận. */
            shutdown(p->conn.fd, SHUT_WR);
            char junk[4096];
            while (recv(p->conn.fd, junk, sizeof(junk), 0) > 0)
                ;
        }
        handle_disconnect(p);
        peers_remove(i);
    }
}

int main(int argc, char **argv)
{
    uint16_t port;

    if (argc != 2 || parse_port(argv[1], &port) < 0) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = tcp_listen(port, 64);
    if (lfd < 0 || set_nonblock(lfd) < 0)
        return 1;
    log_msg(TAG, "lắng nghe cổng %u", port);

    struct pollfd pfd[MAX_PEERS + 1];

    while (!g_stop) {
        size_t n = peers_count();

        pfd[0].fd = lfd;
        pfd[0].events = POLLIN;
        for (size_t i = 0; i < n; i++) {
            peer_t *p = peers_at(i);
            pfd[i + 1].fd = p->conn.fd;
            pfd[i + 1].events =
                POLLIN | (conn_wants_write(&p->conn) ? POLLOUT : 0);
            pfd[i + 1].revents = 0;
        }

        int rc = poll(pfd, n + 1, POLL_TICK_MS);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* Bảng peer không đổi chỉ số cho tới reap_closed nên pfd[i+1] khớp peer i */
        for (size_t i = 0; i < n; i++) {
            peer_t *p = peers_at(i);
            if (!p->closing && pfd[i + 1].revents)
                on_events(p, pfd[i + 1].revents);
        }

        int64_t now = now_ms();
        check_timeouts(now);
        handlers_tick(now);
        reap_closed();

        if (pfd[0].revents & POLLIN)
            accept_all(lfd);
    }

    log_msg(TAG, "dừng");
    while (peers_count() > 0)
        peers_remove(0);
    close(lfd);
    return 0;
}
