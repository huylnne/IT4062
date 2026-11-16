#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "upload.h"
#include "crc32.h"
#include "netio.h"
#include "protocol.h"
#include "sha256.h"
#include "shares.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Ngừng đẩy chunk khi hàng đợi gửi tới Tracker đã có từng này byte */
#define UPLOAD_QBYTES (256 * 1024)

/**
 * Trạng thái từng chunk phía nguồn.
 */
enum {
    CH_UNSENT = 0,
    CH_INFLIGHT = 1, /* đã gửi, chờ ACK */
    CH_ACKED = 2,
    CH_RESEND = 3    /* bị NACK/REQ_RANGE hoặc quá hạn ACK, chờ gửi lại */
};

/**
 * Một phiên gửi. Lần gửi đầu đi đúng thứ tự 0..N-1 nên SHA-256 được tính
 * dần trong lúc gửi, không phải đọc cả file trước.
 */
typedef struct {
    int used;
    uint32_t tid;
    int fd;
    char name[MAX_FILENAME + 1];
    char requester[MAX_USERNAME + 1];
    uint64_t size;
    uint32_t nchunks;
    uint8_t *state;
    uint8_t *tries;     /* số lần đã gửi lại từng chunk */
    int64_t *sent_at;
    uint32_t base;      /* chunk nhỏ nhất chưa ACK */
    uint32_t next_new;  /* chunk chưa gửi lần nào */
    uint32_t inflight;  /* đã gửi, chưa ACK: giới hạn bởi WINDOW_W */
    uint32_t nresend;   /* số chunk đang ở CH_RESEND */
    uint32_t acked;
    uint32_t resent;    /* thống kê: số lần gửi lại */
    int done_sent;
    sha256_ctx_t sha;
    int64_t started;
} upload_t;

static upload_t g_up[MAX_UPLOADS];

static upload_t *find(uint32_t tid)
{
    for (int i = 0; i < MAX_UPLOADS; i++)
        if (g_up[i].used && g_up[i].tid == tid)
            return &g_up[i];
    return NULL;
}

static void up_free(upload_t *u)
{
    if (u->fd >= 0)
        close(u->fd);
    free(u->state);
    free(u->tries);
    free(u->sent_at);
    memset(u, 0, sizeof(*u));
    u->fd = -1;
}

/**
 * @function up_fail: Báo lỗi cho Tracker (ERR kèm tid) rồi đóng phiên.
 *
 * @param ctx: Ngữ cảnh.
 * @param u: Phiên.
 * @param code: Mã lỗi.
 * @param msg: Mô tả.
 *
 * @return: void
 */
static void up_fail(peer_ctx_t *ctx, upload_t *u, uint16_t code,
                    const char *msg)
{
    printf("[gửi] phiên %u (%s -> %s) lỗi: %s\n", u->tid, u->name,
           u->requester, msg);
    conn_send_err(&ctx->conn, u->tid, code, msg);
    up_free(u);
}

static uint32_t chunk_len(const upload_t *u, uint32_t cid)
{
    uint64_t off = (uint64_t)cid * CHUNK_SIZE;
    uint64_t left = u->size - off;
    return left < CHUNK_SIZE ? (uint32_t)left : CHUNK_SIZE;
}

/**
 * @function send_chunk: Đọc chunk cid từ đĩa và gửi. Tiêm lỗi drop/corrupt
 * chỉ áp dụng cho lần gửi đầu.
 *
 * @param ctx: Ngữ cảnh.
 * @param u: Phiên.
 * @param cid: Số thứ tự chunk.
 * @param first: 1 nếu là lần gửi đầu (cập nhật SHA-256).
 * @param now: Thời điểm hiện tại.
 *
 * @return: 0 nếu thành công, -1 nếu đã huỷ phiên vì lỗi đọc file.
 */
static int send_chunk(peer_ctx_t *ctx, upload_t *u, uint32_t cid, int first,
                      int64_t now)
{
    static uint8_t buf[CHUNK_SIZE];
    uint32_t len = chunk_len(u, cid);
    uint8_t *p = buf;
    size_t left = len;
    off_t off = (off_t)cid * CHUNK_SIZE;

    while (left > 0) {
        ssize_t k = pread(u->fd, p, left, off);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0 || (size_t)k > left) {
            up_fail(ctx, u, E_FILE_IO, "không đọc được file nguồn");
            return -1;
        }
        p += k;
        left -= (size_t)k;
        off += k;
    }
    if (first)
        sha256_update(&u->sha, buf, len);

    frame_hdr_t h;
    hdr_init(&h, MSG_CHUNK, u->tid, cid);
    h.length = len;
    h.crc32 = crc32_calc(buf, len);

    const fault_t *f = &ctx->fault;
    if (first && fault_has(f->drop, f->ndrop, cid)) {
        printf("[fault] bỏ chunk %u\n", cid);
    } else {
        if (first && fault_has(f->corrupt, f->ncorrupt, cid)) {
            /* Sửa dữ liệu sau khi đã tính CRC để bên nhận phát hiện */
            buf[len / 2] ^= 0xFF;
            printf("[fault] làm hỏng chunk %u\n", cid);
        }
        if (conn_send_hdr(&ctx->conn, &h, buf) != CONN_OK) {
            up_fail(ctx, u, E_FILE_IO, "hàng đợi gửi đầy");
            return -1;
        }
    }

    u->state[cid] = CH_INFLIGHT;
    u->sent_at[cid] = now;
    return 0;
}

void upload_on_send_req(peer_ctx_t *ctx, uint32_t tid, const uint8_t *payload,
                        uint32_t len)
{
    char requester[MAX_USERNAME + 1];
    char name[MAX_FILENAME + 1];
    rbuf_t r;

    rbuf_init(&r, payload, len);
    int a = get_str(&r, requester, sizeof(requester));
    int b = get_str(&r, name, sizeof(name));
    uint64_t size = get_u64(&r);
    if (a < 0 || b < 0 || !rbuf_done(&r)) {
        conn_send_err(&ctx->conn, tid, E_BAD_FRAME, "SEND_REQ sai khuôn");
        return;
    }

    const share_t *s = shares_find(name);
    if (!s) {
        conn_send_err(&ctx->conn, tid, E_NOT_FOUND, "không còn chia sẻ file");
        return;
    }

    upload_t *u = NULL;
    for (int i = 0; i < MAX_UPLOADS && !u; i++)
        if (!g_up[i].used)
            u = &g_up[i];
    if (!u) {
        conn_send_err(&ctx->conn, tid, E_FULL, "nguồn đang bận");
        return;
    }

    int fd = open(s->path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0 || (uint64_t)st.st_size != size) {
        if (fd >= 0)
            close(fd);
        printf("[gửi] %s: file đã bị xoá hoặc thay đổi dung lượng\n", name);
        conn_send_err(&ctx->conn, tid, E_FILE_IO,
                      "file nguồn bị xoá hoặc thay đổi");
        return;
    }

    memset(u, 0, sizeof(*u));
    u->used = 1;
    u->tid = tid;
    u->fd = fd;
    snprintf(u->name, sizeof(u->name), "%s", name);
    snprintf(u->requester, sizeof(u->requester), "%s", requester);
    u->size = size;
    u->nchunks = (uint32_t)((size + CHUNK_SIZE - 1) / CHUNK_SIZE);
    u->state = calloc(u->nchunks, 1);
    u->tries = calloc(u->nchunks, 1);
    u->sent_at = calloc(u->nchunks, sizeof(int64_t));
    if (!u->state || !u->tries || !u->sent_at) {
        up_fail(ctx, u, E_FILE_IO, "hết bộ nhớ");
        return;
    }
    sha256_init(&u->sha);
    u->started = now_ms();

    char hs[32];
    printf("[gửi] phiên %u: bắt đầu gửi %s (%s, %u chunk) cho %s\n", tid,
           name, human_size(size, hs, sizeof(hs)), u->nchunks, requester);
}

int upload_owns(uint32_t tid)
{
    return find(tid) != NULL;
}

/**
 * @function mark_resend: Đánh dấu chunk đã gửi cần gửi lại (bỏ qua chunk
 * chưa gửi lần nào, đã ACK hoặc đã nằm trong hàng gửi lại).
 *
 * @param u: Phiên.
 * @param cid: Số thứ tự chunk.
 *
 * @return: void
 */
static void mark_resend(upload_t *u, uint32_t cid)
{
    if (cid < u->nchunks && u->state[cid] == CH_INFLIGHT) {
        u->state[cid] = CH_RESEND;
        u->nresend++;
    }
}

static void on_ack(upload_t *u, uint32_t cid)
{
    if (cid >= u->nchunks || u->state[cid] == CH_UNSENT ||
        u->state[cid] == CH_ACKED)
        return;
    if (u->state[cid] == CH_RESEND)
        u->nresend--;
    u->state[cid] = CH_ACKED;
    u->acked++;
    u->inflight--;
    while (u->base < u->nchunks && u->state[u->base] == CH_ACKED)
        u->base++;
}

void upload_on_frame(peer_ctx_t *ctx, const frame_hdr_t *h,
                     const uint8_t *payload)
{
    upload_t *u = find(h->transfer_id);
    if (!u)
        return;

    switch (h->type) {
    case MSG_ACK:
        on_ack(u, h->chunk_id);
        break;
    case MSG_NACK:
        printf("[gửi] phiên %u: NACK chunk %u, gửi lại\n", u->tid,
               h->chunk_id);
        mark_resend(u, h->chunk_id);
        break;
    case MSG_REQ_RANGE: {
        rbuf_t r;
        rbuf_init(&r, payload, h->length);
        uint32_t last = get_u32(&r);
        if (!rbuf_done(&r) || last < h->chunk_id)
            break;
        if (last >= u->nchunks)
            last = u->nchunks - 1;
        printf("[gửi] phiên %u: REQ_RANGE %u-%u, gửi lại\n", u->tid,
               h->chunk_id, last);
        for (uint32_t c = h->chunk_id; c <= last; c++)
            mark_resend(u, c);
        break;
    }
    case MSG_ABORT:
        printf("[gửi] phiên %u (%s -> %s): bị huỷ (ABORT)\n", u->tid,
               u->name, u->requester);
        up_free(u);
        break;
    case MSG_OK: {
        double secs = (double)(now_ms() - u->started) / 1000.0;
        printf("[gửi] phiên %u: %s đã tới %s đầy đủ (%.2f s, gửi lại %u "
               "chunk)\n", u->tid, u->name, u->requester, secs, u->resent);
        up_free(u);
        break;
    }
    case MSG_ERR: {
        char msg[256] = "";
        rbuf_t r;
        rbuf_init(&r, payload, h->length);
        uint16_t code = get_u16(&r);
        get_str(&r, msg, sizeof(msg));
        printf("[gửi] phiên %u: bên nhận báo lỗi %s: %s\n", u->tid,
               err_code_name(code), msg);
        up_free(u);
        break;
    }
    default:
        break;
    }
    (void)ctx;
}

/**
 * @function pump: Đẩy chunk vào hàng đợi: ưu tiên chunk cần gửi lại, sau đó
 * chunk mới khi số chunk chưa ACK < WINDOW_W; gửi xong lần đầu thì gửi DONE.
 *
 * @param ctx: Ngữ cảnh.
 * @param u: Phiên.
 * @param now: Thời điểm hiện tại.
 *
 * @return: void
 */
static void pump(peer_ctx_t *ctx, upload_t *u, int64_t now)
{
    while (u->used && ctx->conn.qbytes < UPLOAD_QBYTES) {
        if (u->nresend > 0) {
            uint32_t cid = u->base;
            while (cid < u->next_new && u->state[cid] != CH_RESEND)
                cid++;
            if (cid < u->next_new) {
                u->nresend--;
                if (++u->tries[cid] > MAX_RETRY) {
                    char msg[64];
                    snprintf(msg, sizeof(msg),
                             "chunk %u quá %d lần gửi lại", cid, MAX_RETRY);
                    up_fail(ctx, u, E_TIMEOUT, msg);
                    return;
                }
                u->resent++;
                if (send_chunk(ctx, u, cid, 0, now) < 0)
                    return;
                continue;
            }
            u->nresend = 0;
        }
        if (u->next_new < u->nchunks && u->inflight < WINDOW_W) {
            uint32_t cid = u->next_new++;
            u->inflight++;
            if (send_chunk(ctx, u, cid, 1, now) < 0)
                return;
            continue;
        }
        break;
    }

    if (u->used && u->next_new == u->nchunks && !u->done_sent) {
        uint8_t digest[SHA256_LEN];
        sha256_final(&u->sha, digest);
        conn_send(&ctx->conn, MSG_DONE, 0, u->tid, 0, digest, SHA256_LEN);
        u->done_sent = 1;
    }
}

void upload_tick(peer_ctx_t *ctx, int64_t now)
{
    for (int i = 0; i < MAX_UPLOADS; i++) {
        upload_t *u = &g_up[i];
        if (!u->used)
            continue;

        for (uint32_t c = u->base; c < u->next_new; c++) {
            if (u->state[c] == CH_INFLIGHT &&
                now - u->sent_at[c] > ACK_TIMEOUT_MS) {
                printf("[gửi] phiên %u: chunk %u quá %d ms chưa có ACK\n",
                       u->tid, c, ACK_TIMEOUT_MS);
                mark_resend(u, c);
            }
        }
        pump(ctx, u, now);
    }
}

int upload_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_UPLOADS; i++)
        n += g_up[i].used;
    return n;
}

void upload_status(void)
{
    for (int i = 0; i < MAX_UPLOADS; i++) {
        const upload_t *u = &g_up[i];
        if (!u->used)
            continue;
        printf("  gửi  #%u %s -> %s: %u/%u chunk đã ACK, gửi lại %u\n",
               u->tid, u->name, u->requester, u->acked, u->nchunks,
               u->resent);
    }
}

void upload_close_all(void)
{
    for (int i = 0; i < MAX_UPLOADS; i++)
        if (g_up[i].used)
            up_free(&g_up[i]);
}
