#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "download.h"
#include "crc32.h"
#include "netio.h"
#include "protocol.h"
#include "sha256.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Chu kỳ in tiến trình (ms) */
#define PROGRESS_MS 500

/**
 * Trạng thái từng chunk phía nhận.
 */
enum {
    RX_NONE = 0,
    RX_HAVE = 1,     /* đã ghi xuống đĩa */
    RX_REQUESTED = 2 /* đã xin gửi lại (NACK/REQ_RANGE), đang chờ */
};

/**
 * Lượt tải đang chạy (mỗi lúc một lượt).
 */
typedef struct {
    int active;
    uint32_t tid;
    char owner[MAX_USERNAME + 1];
    char name[MAX_FILENAME + 1];
    char part[PATH_MAX];
    int fd;
    uint64_t size;
    uint32_t nchunks;
    uint8_t *rx;
    uint32_t count;         /* số chunk đã ghi */
    uint64_t bytes;         /* số byte đã ghi */
    uint32_t next_expected; /* chunk mới tiếp theo (TCP giữ thứ tự) */
    int have_done;
    uint8_t sha[SHA256_LEN];
    uint32_t nacks;
    uint32_t ranges;
    uint32_t dups;
    int64_t started;
    int64_t last_print;
    int last_quarter;
    /* Hàng ACK bị làm chậm (--fault-delay) */
    uint32_t *dq_cid;
    int64_t *dq_due;
    size_t dq_head;
    size_t dq_len;
} download_t;

static download_t g_dl = { .fd = -1 };
static int g_failures;

int download_active(void)
{
    return g_dl.active;
}

int download_owns(uint32_t tid)
{
    return g_dl.active && g_dl.tid == tid;
}

int download_failures(void)
{
    return g_failures;
}

/**
 * @function dl_end: Kết thúc lượt tải; ok = 0 thì xoá .part.
 *
 * @param ok: 1 nếu thành công.
 *
 * @return: void
 */
static void dl_end(int ok)
{
    if (g_dl.fd >= 0)
        close(g_dl.fd);
    if (!ok) {
        unlink(g_dl.part);
        g_failures++;
    }
    free(g_dl.rx);
    free(g_dl.dq_cid);
    free(g_dl.dq_due);
    memset(&g_dl, 0, sizeof(g_dl));
    g_dl.fd = -1;
}

/**
 * @function dl_fail: In lỗi, xoá .part.
 *
 * @param why: Lý do.
 *
 * @return: void
 */
static void dl_fail(const char *why)
{
    printf("\n[tải] %s/%s thất bại: %s (đã xoá file .part)\n", g_dl.owner,
           g_dl.name, why);
    dl_end(0);
}

static uint32_t chunk_len(uint32_t cid)
{
    uint64_t left = g_dl.size - (uint64_t)cid * CHUNK_SIZE;
    return left < CHUNK_SIZE ? (uint32_t)left : CHUNK_SIZE;
}

/**
 * @function mkdir_p: Tạo thư mục (một cấp), bỏ qua nếu đã có.
 *
 * @param dir: Đường dẫn.
 *
 * @return: 0 nếu thư mục tồn tại sau khi gọi, -1 nếu lỗi.
 */
static int mkdir_p(const char *dir)
{
    if (mkdir(dir, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
}

int download_start(peer_ctx_t *ctx, uint32_t tid, const uint8_t *payload,
                   uint32_t len, const char *owner, const char *name)
{
    char o[MAX_USERNAME + 1];
    char n[MAX_FILENAME + 1];
    rbuf_t r;

    rbuf_init(&r, payload, len);
    int a = get_str(&r, o, sizeof(o));
    int b = get_str(&r, n, sizeof(n));
    uint64_t size = get_u64(&r);
    uint32_t nchunks = get_u32(&r);
    uint32_t csize = get_u32(&r);

    /* Không tin dữ liệu từ mạng: tên phải đúng cái đã xin và hợp lệ */
    if (a < 0 || b < 0 || !rbuf_done(&r) || strcmp(o, owner) != 0 ||
        strcmp(n, name) != 0 || !valid_filename(n) || size == 0 ||
        size > MAX_FILE_SIZE || csize != CHUNK_SIZE ||
        nchunks != (size + CHUNK_SIZE - 1) / CHUNK_SIZE) {
        printf("[tải] GET_OK không hợp lệ, huỷ\n");
        peer_send(ctx, MSG_CANCEL, tid, 0, NULL, 0);
        return -1;
    }

    memset(&g_dl, 0, sizeof(g_dl));
    g_dl.fd = -1;
    g_dl.tid = tid;
    snprintf(g_dl.owner, sizeof(g_dl.owner), "%s", o);
    snprintf(g_dl.name, sizeof(g_dl.name), "%s", n);
    g_dl.size = size;
    g_dl.nchunks = nchunks;

    if (mkdir_p(ctx->dl_dir) < 0 ||
        snprintf(g_dl.part, sizeof(g_dl.part), "%s/%s.part", ctx->dl_dir,
                 n) >= (int)sizeof(g_dl.part)) {
        printf("[tải] không tạo được thư mục %s\n", ctx->dl_dir);
        peer_send(ctx, MSG_CANCEL, tid, 0, NULL, 0);
        g_failures++;
        return -1;
    }

    g_dl.fd = open(g_dl.part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    g_dl.rx = calloc(nchunks, 1);
    if (ctx->fault.delay_ms > 0) {
        g_dl.dq_cid = calloc(nchunks, sizeof(uint32_t));
        g_dl.dq_due = calloc(nchunks, sizeof(int64_t));
    }
    if (g_dl.fd < 0 || !g_dl.rx ||
        (ctx->fault.delay_ms > 0 && (!g_dl.dq_cid || !g_dl.dq_due))) {
        printf("[tải] không mở được %s: %s\n", g_dl.part, strerror(errno));
        peer_send(ctx, MSG_CANCEL, tid, 0, NULL, 0);
        dl_end(0);
        return -1;
    }

    g_dl.active = 1;
    g_dl.started = now_ms();
    g_dl.last_print = g_dl.started;

    char hs[32];
    printf("[tải] phiên %u: bắt đầu tải %s/%s (%s, %u chunk)\n", tid, o, n,
           human_size(size, hs, sizeof(hs)), nchunks);
    return 0;
}

/**
 * @function send_ack: Gửi ACK ngay, hoặc xếp vào hàng chờ nếu --fault-delay.
 *
 * @param ctx: Ngữ cảnh.
 * @param cid: Chunk.
 *
 * @return: void
 */
static void send_ack(peer_ctx_t *ctx, uint32_t cid)
{
    if (ctx->fault.delay_ms > 0 && g_dl.dq_len < g_dl.nchunks) {
        size_t pos = (g_dl.dq_head + g_dl.dq_len) % g_dl.nchunks;
        g_dl.dq_cid[pos] = cid;
        g_dl.dq_due[pos] = now_ms() + ctx->fault.delay_ms;
        g_dl.dq_len++;
        return;
    }
    peer_send(ctx, MSG_ACK, g_dl.tid, cid, NULL, 0);
}

/**
 * @function request_range: Xin gửi lại các chunk a..b và đánh dấu đã xin.
 *
 * @param ctx: Ngữ cảnh.
 * @param a: Chunk đầu.
 * @param b: Chunk cuối (>= a).
 *
 * @return: void
 */
static void request_range(peer_ctx_t *ctx, uint32_t a, uint32_t b)
{
    uint8_t buf[4];
    wbuf_t w;
    wbuf_init(&w, buf, sizeof(buf));
    put_u32(&w, b);

    for (uint32_t c = a; c <= b; c++)
        if (g_dl.rx[c] == RX_NONE)
            g_dl.rx[c] = RX_REQUESTED;
    g_dl.ranges++;
    printf("[tải] thiếu chunk %u-%u, gửi REQ_RANGE\n", a, b);
    peer_send(ctx, MSG_REQ_RANGE, g_dl.tid, a, buf, 4);
}

/**
 * @function pick_final_name: Chọn đường dẫn đích chưa tồn tại:
 * "a.txt", "a (1).txt", "a (2).txt", ...
 *
 * @param dir: Thư mục tải về.
 * @param name: Tên file.
 * @param out: Bộ đệm PATH_MAX.
 *
 * @return: 0 nếu tìm được, -1 nếu không.
 */
static int pick_final_name(const char *dir, const char *name, char *out)
{
    struct stat st;
    const char *dot = strrchr(name, '.');
    size_t stem = (dot && dot != name) ? (size_t)(dot - name) : strlen(name);
    const char *ext = name + stem;

    snprintf(out, PATH_MAX, "%s/%s", dir, name);
    if (lstat(out, &st) < 0)
        return 0;
    for (int k = 1; k < 1000; k++) {
        snprintf(out, PATH_MAX, "%s/%.*s (%d)%s", dir, (int)stem, name, k,
                 ext);
        if (lstat(out, &st) < 0)
            return 0;
    }
    return -1;
}

/**
 * @function finish_if_complete: Đủ chunk và đã có DONE thì kiểm SHA-256,
 * đổi tên .part và báo kết quả cho nguồn (OK hoặc ERR kèm tid).
 *
 * @param ctx: Ngữ cảnh.
 *
 * @return: void
 */
static void finish_if_complete(peer_ctx_t *ctx)
{
    if (g_dl.count < g_dl.nchunks || !g_dl.have_done)
        return;

    /* Gửi nốt ACK đang bị làm chậm trước khi kết thúc */
    while (g_dl.dq_len > 0) {
        peer_send(ctx, MSG_ACK, g_dl.tid, g_dl.dq_cid[g_dl.dq_head], NULL, 0);
        g_dl.dq_head = (g_dl.dq_head + 1) % g_dl.nchunks;
        g_dl.dq_len--;
    }

    close(g_dl.fd);
    g_dl.fd = -1;

    uint8_t digest[SHA256_LEN];
    if (g_dl.bytes != g_dl.size || sha256_file(g_dl.part, digest) < 0 ||
        memcmp(digest, g_dl.sha, SHA256_LEN) != 0) {
        conn_send_err(&ctx->conn, g_dl.tid, E_HASH_MISMATCH,
                      "SHA-256 không khớp");
        dl_fail("SHA-256 không khớp");
        return;
    }

    char final[PATH_MAX];
    if (pick_final_name(ctx->dl_dir, g_dl.name, final) < 0 ||
        rename(g_dl.part, final) < 0) {
        conn_send_err(&ctx->conn, g_dl.tid, E_FILE_IO, "không đổi tên được");
        dl_fail("không đổi tên được file .part");
        return;
    }
    peer_send(ctx, MSG_OK, g_dl.tid, 0, NULL, 0);

    char hex[2 * SHA256_LEN + 1];
    char hs[32], sp[32];
    double secs = (double)(now_ms() - g_dl.started) / 1000.0;
    sha256_hex(digest, hex);
    printf("\n[tải] xong: %s (%s, %.2f s, %s/s)\n", final,
           human_size(g_dl.size, hs, sizeof(hs)), secs,
           human_size((uint64_t)(g_dl.size / (secs > 0.001 ? secs : 0.001)),
                      sp, sizeof(sp)));
    printf("[tải] SHA-256 %s khớp; NACK %u, REQ_RANGE %u, chunk trùng %u\n",
           hex, g_dl.nacks, g_dl.ranges, g_dl.dups);
    dl_end(1);
}

/**
 * @function on_chunk: Kiểm tra chunk (phạm vi, độ dài, CRC32), ghi đúng vị
 * trí trong .part rồi ACK; lỗi thì NACK; phát hiện chunk bị bỏ sót thì
 * REQ_RANGE.
 *
 * @param ctx: Ngữ cảnh.
 * @param h: Header.
 * @param payload: Dữ liệu chunk.
 *
 * @return: void
 */
static void on_chunk(peer_ctx_t *ctx, const frame_hdr_t *h,
                     const uint8_t *payload)
{
    uint32_t cid = h->chunk_id;

    if (cid >= g_dl.nchunks) {
        printf("[tải] chunk %u ngoài phạm vi, bỏ qua\n", cid);
        return;
    }

    /* TCP giữ thứ tự: chunk mới nhảy cóc nghĩa là nguồn đã bỏ sót đoạn giữa */
    if (cid > g_dl.next_expected)
        request_range(ctx, g_dl.next_expected, cid - 1);
    if (cid >= g_dl.next_expected)
        g_dl.next_expected = cid + 1;

    if (g_dl.rx[cid] == RX_HAVE) {
        g_dl.dups++;
        send_ack(ctx, cid);
        return;
    }

    const char *bad = NULL;
    if (h->length != chunk_len(cid))
        bad = "sai độ dài";
    else if (crc32_calc(payload, h->length) != h->crc32)
        bad = "sai CRC32";
    if (bad) {
        g_dl.nacks++;
        g_dl.rx[cid] = RX_REQUESTED;
        printf("[tải] chunk %u %s, gửi NACK\n", cid, bad);
        peer_send(ctx, MSG_NACK, g_dl.tid, cid, NULL, 0);
        return;
    }

    size_t done = 0;
    while (done < h->length) {
        ssize_t k = pwrite(g_dl.fd, payload + done, h->length - done,
                           (off_t)cid * CHUNK_SIZE + (off_t)done);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0) {
            g_dl.nacks++;
            g_dl.rx[cid] = RX_REQUESTED;
            printf("[tải] ghi chunk %u lỗi: %s, gửi NACK\n", cid,
                   strerror(errno));
            peer_send(ctx, MSG_NACK, g_dl.tid, cid, NULL, 0);
            return;
        }
        done += (size_t)k;
    }

    g_dl.rx[cid] = RX_HAVE;
    g_dl.count++;
    g_dl.bytes += h->length;
    if (g_dl.bytes > g_dl.size) {
        peer_send(ctx, MSG_CANCEL, g_dl.tid, 0, NULL, 0);
        dl_fail("nhận nhiều hơn dung lượng đã khai");
        return;
    }
    send_ack(ctx, cid);
    finish_if_complete(ctx);
}

/**
 * @function on_done: DONE(sha256): lưu mã; còn thiếu thì xin các khoảng
 * chưa có và chưa từng xin.
 *
 * @param ctx: Ngữ cảnh.
 * @param h: Header.
 * @param payload: 32 byte SHA-256.
 *
 * @return: void
 */
static void on_done(peer_ctx_t *ctx, const frame_hdr_t *h,
                    const uint8_t *payload)
{
    if (h->length != SHA256_LEN) {
        peer_send(ctx, MSG_CANCEL, g_dl.tid, 0, NULL, 0);
        dl_fail("DONE sai khuôn");
        return;
    }
    memcpy(g_dl.sha, payload, SHA256_LEN);
    g_dl.have_done = 1;
    g_dl.next_expected = g_dl.nchunks;

    uint32_t c = 0;
    while (c < g_dl.nchunks) {
        if (g_dl.rx[c] != RX_NONE) {
            c++;
            continue;
        }
        uint32_t a = c;
        while (c < g_dl.nchunks && g_dl.rx[c] == RX_NONE)
            c++;
        request_range(ctx, a, c - 1);
    }
    finish_if_complete(ctx);
}

void download_on_frame(peer_ctx_t *ctx, const frame_hdr_t *h,
                       const uint8_t *payload)
{
    if (!download_owns(h->transfer_id))
        return;

    switch (h->type) {
    case MSG_CHUNK:
        on_chunk(ctx, h, payload);
        break;
    case MSG_DONE:
        on_done(ctx, h, payload);
        break;
    case MSG_SOURCE_GONE:
        dl_fail("nguồn đã gỡ file hoặc mất kết nối (SOURCE_GONE)");
        break;
    case MSG_ERR: {
        char msg[256] = "";
        char why[300];
        rbuf_t r;
        rbuf_init(&r, payload, h->length);
        uint16_t code = get_u16(&r);
        get_str(&r, msg, sizeof(msg));
        snprintf(why, sizeof(why), "%s: %s", err_code_name(code), msg);
        dl_fail(why);
        break;
    }
    default:
        break;
    }
}

void download_cancel(peer_ctx_t *ctx)
{
    if (!g_dl.active) {
        printf("Không có lượt tải nào đang chạy\n");
        return;
    }
    peer_send(ctx, MSG_CANCEL, g_dl.tid, 0, NULL, 0);
    dl_fail("người dùng huỷ");
}

void download_tick(peer_ctx_t *ctx, int64_t now)
{
    if (!g_dl.active)
        return;

    while (g_dl.dq_len > 0 && g_dl.dq_due[g_dl.dq_head] <= now) {
        peer_send(ctx, MSG_ACK, g_dl.tid, g_dl.dq_cid[g_dl.dq_head], NULL, 0);
        g_dl.dq_head = (g_dl.dq_head + 1) % g_dl.nchunks;
        g_dl.dq_len--;
    }

    if (now - g_dl.last_print < PROGRESS_MS)
        return;
    g_dl.last_print = now;

    int pct = (int)(g_dl.bytes * 100 / g_dl.size);
    double secs = (double)(now - g_dl.started) / 1000.0;
    char hs[32], sp[32];
    human_size(g_dl.bytes, hs, sizeof(hs));
    human_size((uint64_t)(g_dl.bytes / (secs > 0.001 ? secs : 0.001)), sp,
               sizeof(sp));

    if (ctx->interactive) {
        char bar[21];
        int fill = pct / 5;
        for (int i = 0; i < 20; i++)
            bar[i] = i < fill ? '#' : '.';
        bar[20] = '\0';
        printf("\r[%s] %3d%% %s  %s/s ", bar, pct, hs, sp);
        fflush(stdout);
    } else if (pct / 25 > g_dl.last_quarter) {
        /* Không phải terminal (script): chỉ in mỗi 25% */
        g_dl.last_quarter = pct / 25;
        printf("[tải] %d%% (%s, %s/s)\n", pct, hs, sp);
    }
}

void download_status(void)
{
    if (!g_dl.active)
        return;
    printf("  tải  #%u %s/%s: %u/%u chunk, NACK %u, REQ_RANGE %u\n",
           g_dl.tid, g_dl.owner, g_dl.name, g_dl.count, g_dl.nchunks,
           g_dl.nacks, g_dl.ranges);
}

void download_abort_local(void)
{
    if (g_dl.active)
        dl_fail("thoát giữa chừng");
}
