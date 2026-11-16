#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

/*
 * fake_peer: peer giả chạy theo kịch bản để test Tracker.
 *
 *   ./bin/fake_peer <host> <port> [tag] < script.txt
 *
 * Mỗi dòng kịch bản là một lệnh (dòng trống và dòng bắt đầu bằng # bị bỏ qua):
 *   JOIN <name>                 UNSHARE <name>
 *   QUIT                        LIST
 *   SHARE <name> <size>         SEARCH <keyword>
 *   GET <owner> <name>
 *   RAW <TYPE>                  gửi frame rỗng loại TYPE, transfer_id = 0
 *   CANCEL                      gửi CANCEL cho phiên của GET_OK gần nhất
 *   BADMAGIC                    gửi header có magic sai
 *   HUGE                        gửi header có length = MAX_PAYLOAD + 1
 *   HALF                        gửi 10 byte đầu của một header rồi thôi
 *   SLEEP <ms>
 *   TIMEOUT <ms>                thời gian chờ cho EXPECT (mặc định 3000)
 *   EXPECT <TYPE> [ERR_NAME]    frame kế tiếp phải là TYPE (và mã lỗi nếu là ERR)
 *   EXPECT_LIST <n>             đọc LIST_RESP tới frame có FLAG_LAST, đếm đủ n mục
 *   EXPECT_CLOSE                Tracker phải đóng kết nối trong thời gian chờ
 *
 * Mã thoát = số EXPECT thất bại (0 = tất cả PASS).
 */

#include "frame.h"
#include "netio.h"
#include "protocol.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_fd;
static int g_timeout_ms = 3000;
static int g_fail;
static int g_closed;
static uint32_t g_last_tid; /* transfer_id của GET_OK/SEND_REQ gần nhất */
static const char *g_tag = "fake_peer";

/**
 * @function send_frame: Đóng gói và gửi một frame (blocking).
 *
 * @param type: Loại thông điệp.
 * @param payload: Payload.
 * @param len: Độ dài payload.
 *
 * @return: void
 */
static void send_frame(uint8_t type, const void *payload, uint32_t len)
{
    uint8_t hdr[HDR_SIZE];
    frame_hdr_t h;
    hdr_init(&h, type, type == MSG_CANCEL ? g_last_tid : 0, 0);
    h.length = len;
    hdr_pack(&h, hdr);
    if (send_all(g_fd, hdr, HDR_SIZE) < 0 ||
        (len > 0 && send_all(g_fd, payload, len) < 0))
        printf("[%s] send %s: %s\n", g_tag, msg_type_name(type),
               strerror(errno));
}

static void set_rcv_timeout(int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(g_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/**
 * @function recv_frame: Đọc một frame trong thời gian chờ hiện tại.
 *
 * @param h: Header nhận được.
 * @param payload: Bộ đệm MAX_PAYLOAD byte.
 *
 * @return: 1 nếu có frame, 0 nếu Tracker đóng kết nối, -1 nếu hết giờ/lỗi.
 */
static int recv_frame(frame_hdr_t *h, uint8_t *payload)
{
    uint8_t hdr[HDR_SIZE];

    if (g_closed)
        return 0;
    set_rcv_timeout(g_timeout_ms);
    ssize_t k = recv_all(g_fd, hdr, HDR_SIZE);
    if (k == 0 || (k < 0 && errno == ECONNRESET)) {
        g_closed = 1;
        return 0;
    }
    if (k < 0)
        return -1;
    if (hdr_unpack(hdr, h) != FRAME_OK) {
        printf("[%s] nhận header hỏng\n", g_tag);
        return -1;
    }
    if (h->length > 0 && recv_all(g_fd, payload, h->length) <= 0)
        return -1;
    if (h->type == MSG_GET_OK || h->type == MSG_SEND_REQ)
        g_last_tid = h->transfer_id;
    return 1;
}

/**
 * @function describe: In frame nhận được (giải mã ERR).
 *
 * @param h: Header.
 * @param payload: Payload.
 * @param code_out: Mã lỗi nếu là ERR (có thể NULL).
 *
 * @return: void
 */
static void describe(const frame_hdr_t *h, const uint8_t *payload,
                     int *code_out)
{
    if (h->type == MSG_ERR) {
        char msg[256] = "";
        rbuf_t r;
        rbuf_init(&r, payload, h->length);
        uint16_t code = get_u16(&r);
        get_str(&r, msg, sizeof(msg));
        printf("[%s] <- ERR %s: %s\n", g_tag, err_code_name(code), msg);
        if (code_out)
            *code_out = code;
    } else {
        printf("[%s] <- %s (tid=%u cid=%u len=%u)\n", g_tag,
               msg_type_name(h->type), h->transfer_id, h->chunk_id,
               h->length);
    }
}

static void pass(const char *what)
{
    printf("[%s] PASS %s\n", g_tag, what);
}

static void fail(const char *what)
{
    printf("[%s] FAIL %s\n", g_tag, what);
    g_fail++;
}

/**
 * @function cmd_expect: EXPECT <TYPE> [ERR_NAME].
 *
 * @param type_name: Tên type mong đợi.
 * @param err_name: Tên mã lỗi mong đợi hoặc NULL.
 * @param line: Dòng kịch bản gốc (để in).
 *
 * @return: void
 */
static void cmd_expect(const char *type_name, const char *err_name,
                       const char *line)
{
    static uint8_t payload[MAX_PAYLOAD];
    frame_hdr_t h;
    int want = msg_type_from_name(type_name);
    int rc = recv_frame(&h, payload);

    if (rc == 0) {
        fail(line);
        printf("[%s]    Tracker đã đóng kết nối\n", g_tag);
        return;
    }
    if (rc < 0) {
        fail(line);
        printf("[%s]    không nhận được frame trong %d ms\n", g_tag,
               g_timeout_ms);
        return;
    }

    int code = 0;
    describe(&h, payload, &code);
    if (h.type != want) {
        fail(line);
        return;
    }
    if (err_name && code != err_code_from_name(err_name)) {
        fail(line);
        return;
    }
    pass(line);
}

/**
 * @function cmd_expect_list: Đọc LIST_RESP nhiều frame, đếm số mục.
 *
 * @param want: Số mục mong đợi.
 * @param line: Dòng kịch bản gốc.
 *
 * @return: void
 */
static void cmd_expect_list(long want, const char *line)
{
    static uint8_t payload[MAX_PAYLOAD];
    long total = 0;

    for (;;) {
        frame_hdr_t h;
        if (recv_frame(&h, payload) != 1 || h.type != MSG_LIST_RESP) {
            fail(line);
            return;
        }
        rbuf_t r;
        rbuf_init(&r, payload, h.length);
        uint16_t count = get_u16(&r);
        for (uint16_t i = 0; i < count; i++) {
            char owner[MAX_USERNAME + 1], name[MAX_FILENAME + 1];
            get_str(&r, owner, sizeof(owner));
            get_str(&r, name, sizeof(name));
            uint64_t size = get_u64(&r);
            if (r.err)
                break;
            printf("[%s]    %s/%s %llu\n", g_tag, owner, name,
                   (unsigned long long)size);
        }
        if (!rbuf_done(&r)) {
            fail(line);
            printf("[%s]    payload LIST_RESP sai khuôn\n", g_tag);
            return;
        }
        total += count;
        if (h.flags & FLAG_LAST)
            break;
    }
    if (total == want)
        pass(line);
    else {
        fail(line);
        printf("[%s]    nhận %ld mục, mong đợi %ld\n", g_tag, total, want);
    }
}

static void cmd_expect_close(const char *line)
{
    static uint8_t payload[MAX_PAYLOAD];
    frame_hdr_t h;
    int rc;

    while ((rc = recv_frame(&h, payload)) == 1)
        describe(&h, payload, NULL);
    if (rc == 0)
        pass(line);
    else
        fail(line);
}

/**
 * @function send_strs: Gửi frame có payload gồm n chuỗi, tuỳ chọn thêm u64.
 *
 * @param type: Loại thông điệp.
 * @param strs: Các chuỗi.
 * @param n: Số chuỗi.
 * @param with_size: 1 nếu thêm u64 size sau các chuỗi.
 * @param size: Giá trị u64.
 *
 * @return: void
 */
static void send_strs(uint8_t type, char **strs, int n, int with_size,
                      uint64_t size)
{
    uint8_t buf[1024];
    wbuf_t w;
    wbuf_init(&w, buf, sizeof(buf));
    for (int i = 0; i < n; i++)
        put_str(&w, strs[i]);
    if (with_size)
        put_u64(&w, size);
    send_frame(type, buf, (uint32_t)w.len);
}

/**
 * @function run_line: Thực hiện một dòng kịch bản.
 *
 * @param line: Dòng (không có \n).
 *
 * @return: void
 */
static void run_line(char *line)
{
    char orig[512];
    char *a[4] = { 0 };
    int n = 0;

    snprintf(orig, sizeof(orig), "%s", line);
    for (char *tok = strtok(line, " \t"); tok && n < 4;
         tok = strtok(NULL, " \t"))
        a[n++] = tok;
    if (n == 0 || a[0][0] == '#')
        return;

    const char *cmd = a[0];
    if (!strcasecmp(cmd, "JOIN") && n == 2) {
        send_strs(MSG_JOIN, &a[1], 1, 0, 0);
    } else if (!strcasecmp(cmd, "QUIT")) {
        send_frame(MSG_QUIT, NULL, 0);
    } else if (!strcasecmp(cmd, "SHARE") && n == 3) {
        send_strs(MSG_SHARE, &a[1], 1, 1, strtoull(a[2], NULL, 10));
    } else if (!strcasecmp(cmd, "UNSHARE") && n == 2) {
        send_strs(MSG_UNSHARE, &a[1], 1, 0, 0);
    } else if (!strcasecmp(cmd, "LIST")) {
        send_frame(MSG_LIST, NULL, 0);
    } else if (!strcasecmp(cmd, "SEARCH") && n == 2) {
        send_strs(MSG_SEARCH, &a[1], 1, 0, 0);
    } else if (!strcasecmp(cmd, "GET") && n == 3) {
        send_strs(MSG_GET, &a[1], 2, 0, 0);
    } else if (!strcasecmp(cmd, "RAW") && n == 2) {
        int t = msg_type_from_name(a[1]);
        if (t < 0)
            t = (int)strtol(a[1], NULL, 0);
        uint8_t hdr[HDR_SIZE];
        frame_hdr_t h;
        hdr_init(&h, (uint8_t)t, 0, 0);
        hdr_pack(&h, hdr);
        send_all(g_fd, hdr, HDR_SIZE);
    } else if (!strcasecmp(cmd, "CANCEL")) {
        send_frame(MSG_CANCEL, NULL, 0);
    } else if (!strcasecmp(cmd, "BADMAGIC")) {
        uint8_t hdr[HDR_SIZE];
        frame_hdr_t h;
        hdr_init(&h, MSG_LIST, 0, 0);
        h.magic = 0xDEAD;
        hdr_pack(&h, hdr);
        send_all(g_fd, hdr, HDR_SIZE);
    } else if (!strcasecmp(cmd, "HUGE")) {
        uint8_t hdr[HDR_SIZE];
        frame_hdr_t h;
        hdr_init(&h, MSG_SHARE, 0, 0);
        h.length = MAX_PAYLOAD + 1;
        hdr_pack(&h, hdr);
        send_all(g_fd, hdr, HDR_SIZE);
    } else if (!strcasecmp(cmd, "HALF")) {
        uint8_t hdr[HDR_SIZE];
        frame_hdr_t h;
        hdr_init(&h, MSG_LIST, 0, 0);
        hdr_pack(&h, hdr);
        send_all(g_fd, hdr, 10);
    } else if (!strcasecmp(cmd, "SLEEP") && n == 2) {
        long ms = strtol(a[1], NULL, 10);
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        return;
    } else if (!strcasecmp(cmd, "TIMEOUT") && n == 2) {
        g_timeout_ms = (int)strtol(a[1], NULL, 10);
        return;
    } else if (!strcasecmp(cmd, "EXPECT") && n >= 2) {
        cmd_expect(a[1], n >= 3 ? a[2] : NULL, orig);
        return;
    } else if (!strcasecmp(cmd, "EXPECT_LIST") && n == 2) {
        cmd_expect_list(strtol(a[1], NULL, 10), orig);
        return;
    } else if (!strcasecmp(cmd, "EXPECT_CLOSE")) {
        cmd_expect_close(orig);
        return;
    } else {
        printf("[%s] lệnh kịch bản không hợp lệ: %s\n", g_tag, orig);
        g_fail++;
        return;
    }
    printf("[%s] -> %s\n", g_tag, orig);
}

int main(int argc, char **argv)
{
    char line[512];

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <host> <port> [tag] < script\n", argv[0]);
        return 100;
    }
    if (argc == 4)
        g_tag = argv[3];

    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    g_fd = tcp_connect(argv[1], argv[2]);
    if (g_fd < 0)
        return 100;

    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        run_line(line);
    }
    close(g_fd);
    return g_fail > 99 ? 99 : g_fail;
}
