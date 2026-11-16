#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "cli.h"
#include "conn.h"
#include "download.h"
#include "fault.h"
#include "frame.h"
#include "netio.h"
#include "peer.h"
#include "protocol.h"
#include "shares.h"
#include "upload.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Chu kỳ poll (ms): đủ ngắn để in tiến trình, kiểm tra timeout */
#define POLL_TICK_MS 100

/* Chờ OK của QUIT tối đa (ms) rồi thoát luôn */
#define QUIT_WAIT_MS 2000

/* Số yêu cầu đang chờ Tracker trả lời */
#define MAX_PENDING 16

/**
 * Yêu cầu đã gửi, chờ trả lời. Tracker trả lời các yêu cầu không thuộc phiên
 * theo đúng thứ tự nhận nên chỉ cần một hàng đợi FIFO để ghép câu trả lời.
 */
enum {
    OP_SHARE,
    OP_UNSHARE,
    OP_LIST,
    OP_GET
};

typedef struct {
    int kind;
    char owner[MAX_USERNAME + 1];
    char name[MAX_FILENAME + 1];
    char path[PATH_MAX];
    uint64_t size;
    uint32_t listed; /* OP_LIST: số mục đã in */
} pending_t;

/**
 * Trạng thái vòng lặp chính.
 */
typedef struct {
    cli_t cli;
    pending_t pend[MAX_PENDING];
    size_t pend_head;
    size_t pend_len;
    int joined;
    int quitting;
    int64_t quit_deadline;
    int stdin_open;
    int waiting;
    int broken;
    int done;
    int exit_code;
} main_state_t;

static peer_ctx_t g_ctx;
static main_state_t g_st;

int peer_send(peer_ctx_t *ctx, uint8_t type, uint32_t tid, uint32_t cid,
              const void *payload, uint32_t len)
{
    if (conn_send(&ctx->conn, type, 0, tid, cid, payload, len) != CONN_OK) {
        g_st.broken = 1;
        return -1;
    }
    return 0;
}

char *human_size(uint64_t n, char *out, size_t cap)
{
    static const char *units[] = { "B", "KB", "MB", "GB" };
    double v = (double)n;
    int u = 0;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        u++;
    }
    if (u == 0)
        snprintf(out, cap, "%llu B", (unsigned long long)n);
    else
        snprintf(out, cap, "%.1f %s", v, units[u]);
    return out;
}

static pending_t *pend_front(void)
{
    return g_st.pend_len ? &g_st.pend[g_st.pend_head] : NULL;
}

static void pend_pop(void)
{
    if (g_st.pend_len) {
        g_st.pend_head = (g_st.pend_head + 1) % MAX_PENDING;
        g_st.pend_len--;
    }
}

/**
 * @function pend_push: Thêm yêu cầu vào cuối hàng chờ.
 *
 * @param kind: Loại (OP_*).
 *
 * @return: Mục mới (đã xoá trắng), hoặc NULL nếu hàng đầy.
 */
static pending_t *pend_push(int kind)
{
    if (g_st.pend_len >= MAX_PENDING) {
        printf("Quá nhiều yêu cầu đang chờ, thử lại sau\n");
        return NULL;
    }
    pending_t *p = &g_st.pend[(g_st.pend_head + g_st.pend_len) % MAX_PENDING];
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    g_st.pend_len++;
    return p;
}

static int pend_has(int kind)
{
    for (size_t i = 0; i < g_st.pend_len; i++)
        if (g_st.pend[(g_st.pend_head + i) % MAX_PENDING].kind == kind)
            return 1;
    return 0;
}

/**
 * @function blocked: Lệnh wait đang chờ (yêu cầu còn treo hoặc đang tải).
 *
 * @return: 1 nếu chưa được xử lý lệnh tiếp theo.
 */
static int blocked(void)
{
    if (g_st.waiting && (g_st.pend_len > 0 || download_active()))
        return 1;
    g_st.waiting = 0;
    return 0;
}

static void prompt(void)
{
    if (g_ctx.interactive && g_st.joined && !g_st.quitting && !blocked()) {
        printf("> ");
        fflush(stdout);
    }
}

/**
 * @function start_quit: Gửi QUIT và chờ OK (tối đa QUIT_WAIT_MS).
 *
 * @return: void
 */
static void start_quit(void)
{
    if (g_st.quitting)
        return;
    g_st.quitting = 1;
    g_st.quit_deadline = now_ms() + QUIT_WAIT_MS;
    download_abort_local();
    peer_send(&g_ctx, MSG_QUIT, 0, 0, NULL, 0);
}

static void print_help(void)
{
    printf("Lệnh:\n"
           "  share <đường dẫn>        công bố file\n"
           "  unshare <tên file>       ngừng chia sẻ\n"
           "  list                     xem mọi file đang chia sẻ\n"
           "  search <từ khoá>         tìm file theo tên\n"
           "  get <chủ file> <tên>     tải file về %s/\n"
           "  cancel                   huỷ lượt tải đang chạy\n"
           "  status                   file đang chia sẻ, phiên đang truyền\n"
           "  wait                     chờ yêu cầu và lượt tải xong (cho script)\n"
           "  help                     in hướng dẫn này\n"
           "  quit                     thoát\n",
           g_ctx.dl_dir);
}

/**
 * @function cmd_share: share <đường dẫn>.
 *
 * @param path: Đường dẫn do người dùng nhập.
 *
 * @return: void
 */
static void cmd_share(const char *path)
{
    char abs[PATH_MAX];
    struct stat st;

    if (!realpath(path, abs) || stat(abs, &st) < 0) {
        printf("Không mở được %s: %s\n", path, strerror(errno));
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        printf("%s không phải file thường\n", path);
        return;
    }
    if (st.st_size <= 0 || (uint64_t)st.st_size > MAX_FILE_SIZE) {
        printf("Dung lượng phải trong (0, 2 GiB]\n");
        return;
    }

    const char *name = strrchr(abs, '/');
    name = name ? name + 1 : abs;
    if (!valid_filename(name)) {
        printf("Tên file không hợp lệ: %s\n", name);
        return;
    }
    if (shares_find(name)) {
        printf("Đã chia sẻ một file tên %s rồi\n", name);
        return;
    }

    pending_t *p = pend_push(OP_SHARE);
    if (!p)
        return;
    snprintf(p->name, sizeof(p->name), "%.255s", name);
    snprintf(p->path, sizeof(p->path), "%s", abs);
    p->size = (uint64_t)st.st_size;

    uint8_t buf[1 + MAX_FILENAME + 8];
    wbuf_t w;
    wbuf_init(&w, buf, sizeof(buf));
    put_str(&w, name);
    put_u64(&w, p->size);
    peer_send(&g_ctx, MSG_SHARE, 0, 0, buf, (uint32_t)w.len);
}

/**
 * @function send_one_str: Gửi frame có payload là một chuỗi.
 *
 * @param type: Loại thông điệp.
 * @param s: Chuỗi (<= 255 byte).
 *
 * @return: 0 nếu gửi được, -1 nếu chuỗi quá dài.
 */
static int send_one_str(uint8_t type, const char *s)
{
    uint8_t buf[1 + 255];
    wbuf_t w;
    wbuf_init(&w, buf, sizeof(buf));
    put_str(&w, s);
    if (w.err) {
        printf("Chuỗi quá dài (tối đa 255 byte)\n");
        return -1;
    }
    return peer_send(&g_ctx, type, 0, 0, buf, (uint32_t)w.len);
}

static void cmd_unshare(const char *name)
{
    if (!shares_find(name)) {
        printf("Bạn không chia sẻ file tên %s\n", name);
        return;
    }
    pending_t *p = pend_push(OP_UNSHARE);
    if (!p)
        return;
    snprintf(p->name, sizeof(p->name), "%s", name);
    send_one_str(MSG_UNSHARE, name);
}

static void cmd_list(const char *keyword)
{
    if (keyword && strlen(keyword) > 255) {
        printf("Từ khoá quá dài\n");
        return;
    }
    if (!pend_push(OP_LIST))
        return;
    if (keyword)
        send_one_str(MSG_SEARCH, keyword);
    else
        peer_send(&g_ctx, MSG_LIST, 0, 0, NULL, 0);
}

/**
 * @function cmd_get: get <chủ file> <tên file>.
 *
 * @param args: Phần sau "get".
 *
 * @return: void
 */
static void cmd_get(char *args)
{
    char *argv[2];

    if (!args || cli_split(args, argv, 2) != 2) {
        printf("Cú pháp: get <chủ file> <tên file>\n");
        return;
    }
    if (download_active() || pend_has(OP_GET)) {
        printf("Đang có lượt tải khác, chờ xong hoặc cancel\n");
        return;
    }
    if (!valid_username(argv[0]) || !valid_filename(argv[1])) {
        printf("Chủ file hoặc tên file không hợp lệ\n");
        return;
    }

    pending_t *p = pend_push(OP_GET);
    if (!p)
        return;
    snprintf(p->owner, sizeof(p->owner), "%s", argv[0]);
    snprintf(p->name, sizeof(p->name), "%s", argv[1]);

    uint8_t buf[2 + MAX_USERNAME + MAX_FILENAME];
    wbuf_t w;
    wbuf_init(&w, buf, sizeof(buf));
    put_str(&w, p->owner);
    put_str(&w, p->name);
    peer_send(&g_ctx, MSG_GET, 0, 0, buf, (uint32_t)w.len);
}

static void cmd_status(void)
{
    char hs[32];
    printf("Đang chia sẻ %zu file:\n", shares_count());
    for (size_t i = 0; i < shares_count(); i++) {
        const share_t *s = shares_at(i);
        printf("  %-30s %10s  %s\n", s->name,
               human_size(s->size, hs, sizeof(hs)), s->path);
    }
    if (!download_active() && upload_count() == 0)
        printf("Không có phiên truyền nào\n");
    download_status();
    upload_status();
}

/**
 * @function on_command: Xử lý một dòng lệnh người dùng.
 *
 * @param line: Dòng lệnh.
 *
 * @return: void
 */
static void on_command(char *line)
{
    char *argv[2];

    /* argv[0] = tên lệnh, argv[1] = phần còn lại (tách tiếp theo từng lệnh) */
    int argc = cli_split(line, argv, 2);
    if (argc == 0 || argv[0][0] == '#') {
        prompt();
        return;
    }

    const char *cmd = argv[0];
    char *rest = argc > 1 ? argv[1] : NULL;

    if (!strcasecmp(cmd, "quit") || !strcasecmp(cmd, "exit")) {
        start_quit();
        return;
    }
    if (!strcasecmp(cmd, "help")) {
        print_help();
    } else if (!strcasecmp(cmd, "share") && rest) {
        cmd_share(rest);
    } else if (!strcasecmp(cmd, "unshare") && rest) {
        cmd_unshare(rest);
    } else if (!strcasecmp(cmd, "list")) {
        cmd_list(NULL);
    } else if (!strcasecmp(cmd, "search") && rest) {
        cmd_list(rest);
    } else if (!strcasecmp(cmd, "get")) {
        cmd_get(rest);
    } else if (!strcasecmp(cmd, "cancel")) {
        download_cancel(&g_ctx);
    } else if (!strcasecmp(cmd, "status")) {
        cmd_status();
    } else if (!strcasecmp(cmd, "wait")) {
        g_st.waiting = 1;
    } else {
        printf("Lệnh không hợp lệ hoặc thiếu đối số: %s (gõ help)\n", cmd);
    }
    prompt();
}

/**
 * @function read_err: Giải mã ERR (u16 code, str msg).
 *
 * @param payload: Payload.
 * @param len: Độ dài.
 * @param msg: Bộ đệm 256 byte nhận mô tả.
 *
 * @return: Mã lỗi.
 */
static uint16_t read_err(const uint8_t *payload, uint32_t len, char *msg)
{
    rbuf_t r;
    rbuf_init(&r, payload, len);
    uint16_t code = get_u16(&r);
    if (get_str(&r, msg, 256) < 0)
        msg[0] = '\0';
    return code;
}

/**
 * @function on_reply: OK/ERR (không thuộc phiên) trả lời yêu cầu đầu hàng chờ.
 *
 * @param ok: 1 nếu OK, 0 nếu ERR.
 * @param payload: Payload ERR.
 * @param len: Độ dài.
 *
 * @return: void
 */
static void on_reply(int ok, const uint8_t *payload, uint32_t len)
{
    char msg[256] = "";
    uint16_t code = ok ? 0 : read_err(payload, len, msg);

    if (!g_st.joined) {
        if (ok) {
            g_st.joined = 1;
            printf("Đã vào hệ thống với tên %s (gõ help để xem lệnh)\n",
                   g_ctx.username);
        } else {
            printf("Lỗi %s (%u): %s\n", err_code_name(code), code, msg);
            g_st.done = 1;
            g_st.exit_code = 1;
        }
        return;
    }

    pending_t *p = pend_front();
    if (!p) {
        if (ok && g_st.quitting)
            g_st.done = 1;
        else if (!ok)
            printf("Lỗi %s (%u): %s\n", err_code_name(code), code, msg);
        return;
    }

    if (!ok) {
        printf("Lỗi %s (%u): %s\n", err_code_name(code), code, msg);
    } else if (p->kind == OP_SHARE) {
        if (shares_add(p->name, p->path, p->size) < 0)
            printf("Bảng chia sẻ cục bộ đầy\n");
        else
            printf("Đã chia sẻ %s\n", p->name);
    } else if (p->kind == OP_UNSHARE) {
        shares_remove(p->name);
        printf("Đã ngừng chia sẻ %s\n", p->name);
    }
    pend_pop();
}

/**
 * @function on_list_resp: In một frame LIST_RESP.
 *
 * @param h: Header (FLAG_LAST ở frame cuối).
 * @param payload: Payload.
 *
 * @return: void
 */
static void on_list_resp(const frame_hdr_t *h, const uint8_t *payload)
{
    pending_t *p = pend_front();
    if (!p || p->kind != OP_LIST)
        return;

    rbuf_t r;
    rbuf_init(&r, payload, h->length);
    uint16_t count = get_u16(&r);
    if (p->listed == 0 && count > 0)
        printf("%-20s %10s  %s\n", "CHỦ FILE", "DUNG LƯỢNG", "TÊN FILE");
    for (uint16_t i = 0; i < count; i++) {
        char owner[MAX_USERNAME + 1], name[MAX_FILENAME + 1], hs[32];
        if (get_str(&r, owner, sizeof(owner)) < 0 ||
            get_str(&r, name, sizeof(name)) < 0)
            break;
        uint64_t size = get_u64(&r);
        if (r.err)
            break;
        printf("%-20s %10s  %s%s\n", owner, human_size(size, hs, sizeof(hs)),
               name, strcmp(owner, g_ctx.username) == 0 ? "  (của bạn)" : "");
        p->listed++;
    }
    if (h->flags & FLAG_LAST) {
        printf("(%u file)\n", p->listed);
        pend_pop();
    }
}

/**
 * @function on_frame: Xử lý frame từ Tracker (callback cho conn_on_readable).
 *
 * @param c: Kết nối Tracker.
 * @param h: Header.
 * @param payload: Payload.
 * @param ctx: Không dùng.
 *
 * @return: 0 để đọc tiếp, 1 để dừng.
 */
static int on_frame(conn_t *c, const frame_hdr_t *h, const uint8_t *payload,
                    void *ctx)
{
    (void)c;
    (void)ctx;

    /* Frame thuộc phiên: chuyển cho phía tải hoặc phía gửi */
    if (h->transfer_id != 0 && h->type != MSG_SEND_REQ &&
        h->type != MSG_GET_OK) {
        if (download_owns(h->transfer_id))
            download_on_frame(&g_ctx, h, payload);
        else if (upload_owns(h->transfer_id))
            upload_on_frame(&g_ctx, h, payload);
        return g_st.done;
    }

    pending_t *p = pend_front();
    switch (h->type) {
    case MSG_OK:
        on_reply(1, payload, h->length);
        break;
    case MSG_ERR:
        on_reply(0, payload, h->length);
        break;
    case MSG_LIST_RESP:
        on_list_resp(h, payload);
        break;
    case MSG_SEND_REQ:
        upload_on_send_req(&g_ctx, h->transfer_id, payload, h->length);
        break;
    case MSG_GET_OK:
        if (p && p->kind == OP_GET) {
            if (download_active())
                peer_send(&g_ctx, MSG_CANCEL, h->transfer_id, 0, NULL, 0);
            else
                download_start(&g_ctx, h->transfer_id, payload, h->length,
                               p->owner, p->name);
            pend_pop();
        } else {
            peer_send(&g_ctx, MSG_CANCEL, h->transfer_id, 0, NULL, 0);
        }
        break;
    case MSG_BUSY:
        if (p && p->kind == OP_GET) {
            printf("Nguồn %s đang bận (BUSY), thử lại sau\n", p->owner);
            pend_pop();
        }
        break;
    default:
        printf("Nhận %s ngoài dự kiến, bỏ qua\n", msg_type_name(h->type));
        break;
    }
    return g_st.done;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <tracker_host> <port> <username> [tuỳ chọn]\n"
            "  --dl DIR              thư mục lưu file tải về (mặc định download)\n"
            "  --fault-drop=LIST     bỏ lần gửi đầu của các chunk, vd 3,7,10-12\n"
            "  --fault-corrupt=LIST  làm hỏng lần gửi đầu của các chunk\n"
            "  --fault-delay=MS      làm chậm mỗi ACK MS mili giây\n",
            prog);
}

/**
 * @function parse_options: Đọc các tuỳ chọn sau username.
 *
 * @param argc: Số đối số.
 * @param argv: Đối số.
 *
 * @return: 0 nếu hợp lệ, -1 nếu không.
 */
static int parse_options(int argc, char **argv)
{
    g_ctx.dl_dir = "download";
    for (int i = 4; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--dl") && i + 1 < argc) {
            g_ctx.dl_dir = argv[++i];
        } else if (!strncmp(a, "--fault-drop=", 13)) {
            if (fault_parse_list(a + 13, g_ctx.fault.drop,
                                 &g_ctx.fault.ndrop) < 0)
                return -1;
        } else if (!strncmp(a, "--fault-corrupt=", 16)) {
            if (fault_parse_list(a + 16, g_ctx.fault.corrupt,
                                 &g_ctx.fault.ncorrupt) < 0)
                return -1;
        } else if (!strncmp(a, "--fault-delay=", 14)) {
            g_ctx.fault.delay_ms = atoi(a + 14);
            if (g_ctx.fault.delay_ms < 0)
                return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint16_t port;

    if (argc < 4 || parse_port(argv[2], &port) < 0 ||
        parse_options(argc, argv) < 0) {
        usage(argv[0]);
        return 1;
    }
    if (!valid_username(argv[3])) {
        fprintf(stderr, "Username 3-32 ký tự, chỉ chữ, số, gạch dưới\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    int fd = tcp_connect(argv[1], argv[2]);
    if (fd < 0 || set_nonblock(fd) < 0)
        return 1;

    conn_init(&g_ctx.conn, fd, SENDQ_LIMIT);
    g_ctx.username = argv[3];
    g_ctx.interactive = isatty(STDIN_FILENO);
    cli_init(&g_st.cli);
    g_st.stdin_open = 1;

    uint8_t buf[1 + MAX_USERNAME];
    wbuf_t w;
    wbuf_init(&w, buf, sizeof(buf));
    put_str(&w, g_ctx.username);
    peer_send(&g_ctx, MSG_JOIN, 0, 0, buf, (uint32_t)w.len);

    while (!g_st.done && !g_st.broken) {
        char line[CLI_LINE_MAX];

        /* Chỉ xử lý lệnh sau khi JOIN xong, và dừng khi lệnh wait đang chờ */
        while (g_st.joined && !g_st.quitting && !g_st.done && !blocked() &&
               cli_next_line(&g_st.cli, line))
            on_command(line);
        if (g_st.joined && !g_st.stdin_open && g_st.cli.len == 0 &&
            !blocked())
            start_quit();

        struct pollfd pfd[2];
        int nfds = 1;
        pfd[0].fd = g_ctx.conn.fd;
        pfd[0].events =
            POLLIN | (conn_wants_write(&g_ctx.conn) ? POLLOUT : 0);
        if (g_st.stdin_open && cli_has_room(&g_st.cli) && !g_st.quitting) {
            pfd[1].fd = STDIN_FILENO;
            pfd[1].events = POLLIN;
            nfds = 2;
        }

        int rc = poll(pfd, (nfds_t)nfds, POLL_TICK_MS);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        short re = pfd[0].revents;
        if (re & (POLLIN | POLLHUP | POLLERR)) {
            int r = conn_on_readable(&g_ctx.conn, on_frame, NULL);
            if (r == CONN_EOF || r == CONN_ERR || r == CONN_BAD_FRAME) {
                if (!g_st.quitting) {
                    fprintf(stderr, "Mất kết nối tới Tracker\n");
                    g_st.exit_code = 1;
                }
                break;
            }
        }

        if (nfds > 1 && (pfd[1].revents & (POLLIN | POLLHUP)) &&
            cli_read(&g_st.cli, STDIN_FILENO) != 0)
            g_st.stdin_open = 0;

        int64_t now = now_ms();
        if (!g_st.quitting) {
            upload_tick(&g_ctx, now);
            download_tick(&g_ctx, now);
        }

        if (conn_wants_write(&g_ctx.conn) &&
            conn_flush(&g_ctx.conn) != CONN_OK) {
            fprintf(stderr, "Mất kết nối tới Tracker\n");
            g_st.exit_code = 1;
            break;
        }

        if (g_st.quitting && now > g_st.quit_deadline)
            break;
    }

    if (g_st.broken) {
        fprintf(stderr, "Không gửi được tới Tracker\n");
        g_st.exit_code = 1;
    }
    download_abort_local();
    upload_close_all();
    conn_close(&g_ctx.conn);
    if (g_st.exit_code == 0 && download_failures() > 0)
        g_st.exit_code = 2;
    return g_st.exit_code;
}
