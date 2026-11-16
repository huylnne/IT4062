#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "handlers.h"
#include "catalog.h"
#include "netio.h"
#include "protocol.h"
#include "transfer.h"
#include "util.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#define TAG "tracker"

/**
 * @function mark_send: Kiểm tra kết quả xếp frame cho peer p; hàng đợi đầy
 * nghĩa là p không đọc kịp, coi là treo và đóng.
 *
 * @param p: Peer nhận.
 * @param rc: Kết quả conn_send*.
 *
 * @return: void
 */
static void mark_send(peer_t *p, int rc)
{
    if (rc == CONN_OK)
        return;
    log_msg(TAG, "%s (%s): hàng đợi gửi đầy hoặc lỗi, đóng kết nối",
            p->addr, p->username[0] ? p->username : "-");
    p->closing = CLOSE_NOW;
}

/**
 * @function send_simple: Gửi frame không payload (bỏ qua peer đã hỏng).
 *
 * @param p: Peer nhận.
 * @param type: Loại thông điệp.
 * @param tid: Mã phiên.
 *
 * @return: void
 */
static void send_simple(peer_t *p, uint8_t type, uint32_t tid)
{
    if (p->closing == CLOSE_NOW)
        return;
    mark_send(p, conn_send(&p->conn, type, 0, tid, 0, NULL, 0));
}

static void send_err(peer_t *p, uint32_t tid, uint16_t code, const char *msg)
{
    if (p->closing == CLOSE_NOW)
        return;
    mark_send(p, conn_send_err(&p->conn, tid, code, msg));
}

static void reply_ok(peer_t *p)
{
    send_simple(p, MSG_OK, 0);
}

static void reply_err(peer_t *p, uint16_t code, const char *msg)
{
    send_err(p, 0, code, msg);
}

/**
 * @function forward: Chuyển tiếp nguyên frame (giữ crc32 gốc) cho peer to.
 *
 * @param to: Peer nhận.
 * @param h: Header.
 * @param payload: Payload.
 *
 * @return: void
 */
static void forward(peer_t *to, const frame_hdr_t *h, const uint8_t *payload)
{
    if (to->closing == CLOSE_NOW)
        return;
    mark_send(to, conn_send_hdr(&to->conn, h, payload));
}

/**
 * @function contains_ci: Kiểm tra needle là chuỗi con của hay (không phân
 * biệt hoa thường ASCII). needle rỗng luôn khớp.
 *
 * @param hay: Chuỗi lớn.
 * @param needle: Chuỗi cần tìm.
 *
 * @return: 1 nếu khớp, 0 nếu không.
 */
static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (const char *p = hay; ; p++) {
        if (strncasecmp(p, needle, n) == 0)
            return 1;
        if (*p == '\0')
            return 0;
    }
}

/**
 * @function on_join: JOIN (str username).
 *
 * @param p: Peer gửi.
 * @param payload: Payload.
 * @param len: Độ dài payload.
 *
 * @return: void
 */
static void on_join(peer_t *p, const uint8_t *payload, uint32_t len)
{
    char name[MAX_USERNAME + 1];
    rbuf_t r;

    if (p->username[0]) {
        reply_err(p, E_ALREADY_JOINED, "đã JOIN rồi");
        return;
    }
    rbuf_init(&r, payload, len);
    if (get_str(&r, name, sizeof(name)) < 0 || !rbuf_done(&r) ||
        !valid_username(name)) {
        reply_err(p, E_BAD_USERNAME,
                  "username 3-32 ký tự, chỉ chữ, số, gạch dưới");
        return;
    }
    if (peers_find_user(name)) {
        reply_err(p, E_NAME_TAKEN, "username đang được dùng");
        return;
    }

    strcpy(p->username, name);
    log_msg(TAG, "%s JOIN as %s", p->addr, name);
    reply_ok(p);
}

/**
 * @function on_quit: QUIT: trả OK rồi đóng sau khi gửi xong.
 *
 * @param p: Peer gửi.
 *
 * @return: void
 */
static void on_quit(peer_t *p)
{
    log_msg(TAG, "%s (%s) QUIT", p->addr,
            p->username[0] ? p->username : "-");
    reply_ok(p);
    if (!p->closing)
        p->closing = CLOSE_FLUSH;
}

/**
 * @function on_share: SHARE (str name, u64 size).
 *
 * @param p: Peer gửi.
 * @param payload: Payload.
 * @param len: Độ dài payload.
 *
 * @return: void
 */
static void on_share(peer_t *p, const uint8_t *payload, uint32_t len)
{
    char name[MAX_FILENAME + 1];
    rbuf_t r;

    rbuf_init(&r, payload, len);
    int n = get_str(&r, name, sizeof(name));
    uint64_t size = get_u64(&r);
    if (n < 0 || !rbuf_done(&r)) {
        reply_err(p, E_BAD_FRAME, "payload SHARE sai khuôn");
        return;
    }
    if (!valid_filename(name)) {
        reply_err(p, E_BAD_FILENAME, "tên file không hợp lệ");
        return;
    }
    if (size == 0 || size > MAX_FILE_SIZE) {
        reply_err(p, E_BAD_SIZE, "dung lượng phải trong (0, 2 GiB]");
        return;
    }

    int rc = catalog_add(p->username, name, size);
    if (rc == CAT_EXISTS) {
        reply_err(p, E_ALREADY_SHARED, "file này đã được chia sẻ");
        return;
    }
    if (rc == CAT_FULL) {
        reply_err(p, E_FULL, "catalog đầy");
        return;
    }
    log_msg(TAG, "%s SHARE %s (%llu B)", p->username, name,
            (unsigned long long)size);
    reply_ok(p);
}

/**
 * @function end_sessions_of_file: Đóng mọi phiên đang tải (src, name):
 * SOURCE_GONE cho người tải, ABORT cho nguồn.
 *
 * @param src: Peer nguồn.
 * @param name: Tên file.
 *
 * @return: void
 */
static void end_sessions_of_file(peer_t *src, const char *name)
{
    for (size_t i = transfer_count(); i-- > 0;) {
        transfer_t *t = transfer_at(i);
        if (t->src != src || strcmp(t->name, name) != 0)
            continue;
        log_msg(TAG, "phiên %u: nguồn gỡ file, SOURCE_GONE", t->id);
        send_simple(t->dst, MSG_SOURCE_GONE, t->id);
        send_simple(t->src, MSG_ABORT, t->id);
        transfer_close(t);
    }
}

/**
 * @function on_unshare: UNSHARE (str name).
 *
 * @param p: Peer gửi.
 * @param payload: Payload.
 * @param len: Độ dài payload.
 *
 * @return: void
 */
static void on_unshare(peer_t *p, const uint8_t *payload, uint32_t len)
{
    char name[MAX_FILENAME + 1];
    rbuf_t r;

    rbuf_init(&r, payload, len);
    if (get_str(&r, name, sizeof(name)) < 0 || !rbuf_done(&r)) {
        reply_err(p, E_BAD_FRAME, "payload UNSHARE sai khuôn");
        return;
    }
    if (catalog_remove(p->username, name) < 0) {
        reply_err(p, E_NOT_FOUND, "bạn không chia sẻ file này");
        return;
    }
    log_msg(TAG, "%s UNSHARE %s", p->username, name);
    end_sessions_of_file(p, name);
    reply_ok(p);
}

/**
 * @function send_list_frame: Điền count vào 2 byte đầu và gửi LIST_RESP.
 *
 * @param p: Peer nhận.
 * @param w: Payload đang dựng (2 byte đầu dành cho count).
 * @param count: Số mục trong frame.
 * @param flags: FLAG_LAST cho frame cuối.
 *
 * @return: void
 */
static void send_list_frame(peer_t *p, wbuf_t *w, uint16_t count,
                            uint8_t flags)
{
    w->buf[0] = (uint8_t)(count >> 8);
    w->buf[1] = (uint8_t)count;
    if (p->closing != CLOSE_NOW)
        mark_send(p, conn_send(&p->conn, MSG_LIST_RESP, flags, 0, 0, w->buf,
                               (uint32_t)w->len));
}

/**
 * @function on_list: LIST (keyword = NULL) hoặc SEARCH (str keyword).
 * Kết quả chia thành nhiều frame <= MAX_PAYLOAD, frame cuối có FLAG_LAST.
 *
 * @param p: Peer gửi.
 * @param keyword: Từ khoá, NULL nếu lấy tất cả.
 *
 * @return: void
 */
static void on_list(peer_t *p, const char *keyword)
{
    static uint8_t buf[MAX_PAYLOAD];
    wbuf_t w;
    uint16_t count = 0;
    size_t total = 0;

    wbuf_init(&w, buf, sizeof(buf));
    put_u16(&w, 0);

    for (size_t i = 0; i < catalog_count(); i++) {
        const cat_entry_t *e = catalog_at(i);
        if (keyword && !contains_ci(e->name, keyword))
            continue;

        size_t need = 1 + strlen(e->owner) + 1 + strlen(e->name) + 8;
        if (w.len + need > w.cap || count == UINT16_MAX) {
            send_list_frame(p, &w, count, 0);
            wbuf_init(&w, buf, sizeof(buf));
            put_u16(&w, 0);
            count = 0;
        }
        put_str(&w, e->owner);
        put_str(&w, e->name);
        put_u64(&w, e->size);
        count++;
        total++;
    }
    send_list_frame(p, &w, count, FLAG_LAST);
    log_msg(TAG, "%s %s%s%s -> %zu file", p->username,
            keyword ? "SEARCH \"" : "LIST", keyword ? keyword : "",
            keyword ? "\"" : "", total);
}

static void on_search(peer_t *p, const uint8_t *payload, uint32_t len)
{
    char kw[256];
    rbuf_t r;

    rbuf_init(&r, payload, len);
    if (get_str(&r, kw, sizeof(kw)) < 0 || !rbuf_done(&r)) {
        reply_err(p, E_BAD_FRAME, "payload SEARCH sai khuôn");
        return;
    }
    on_list(p, kw);
}

/**
 * @function on_get: GET (str owner, str name): kiểm tra, mở phiên, gửi
 * SEND_REQ cho nguồn và GET_OK cho người tải.
 *
 * @param p: Peer xin tải.
 * @param payload: Payload.
 * @param len: Độ dài payload.
 *
 * @return: void
 */
static void on_get(peer_t *p, const uint8_t *payload, uint32_t len)
{
    char owner[MAX_USERNAME + 1];
    char name[MAX_FILENAME + 1];
    rbuf_t r;

    rbuf_init(&r, payload, len);
    if (get_str(&r, owner, sizeof(owner)) < 0 ||
        get_str(&r, name, sizeof(name)) < 0 || !rbuf_done(&r)) {
        reply_err(p, E_BAD_FRAME, "payload GET sai khuôn");
        return;
    }
    if (strcmp(owner, p->username) == 0) {
        reply_err(p, E_SELF_GET, "không thể tải file của chính mình");
        return;
    }

    peer_t *src = peers_find_user(owner);
    const cat_entry_t *e = catalog_find(owner, name);
    if (!src) {
        reply_err(p, E_OWNER_OFFLINE, "chủ file không online");
        return;
    }
    if (!e) {
        reply_err(p, E_NOT_FOUND, "không có file này trong catalog");
        return;
    }
    if (transfer_count_src(src) >= MAX_SESSIONS_PER_SRC) {
        log_msg(TAG, "%s GET %s/%s -> BUSY", p->username, owner, name);
        send_simple(p, MSG_BUSY, 0);
        return;
    }

    transfer_t *t = transfer_new(src, p, name, e->size);
    if (!t) {
        reply_err(p, E_FULL, "bảng phiên đầy, thử lại sau");
        return;
    }

    uint8_t buf[2 * 256 + 32];
    wbuf_t w;
    uint32_t nchunks = (uint32_t)((e->size + CHUNK_SIZE - 1) / CHUNK_SIZE);

    wbuf_init(&w, buf, sizeof(buf));
    put_str(&w, p->username);
    put_str(&w, name);
    put_u64(&w, e->size);
    mark_send(src, conn_send(&src->conn, MSG_SEND_REQ, 0, t->id, 0, buf,
                             (uint32_t)w.len));

    wbuf_init(&w, buf, sizeof(buf));
    put_str(&w, owner);
    put_str(&w, name);
    put_u64(&w, e->size);
    put_u32(&w, nchunks);
    put_u32(&w, CHUNK_SIZE);
    mark_send(p, conn_send(&p->conn, MSG_GET_OK, 0, t->id, 0, buf,
                           (uint32_t)w.len));

    log_msg(TAG, "phiên %u: %s -> %s, %s (%llu B, %u chunk)", t->id, owner,
            p->username, name, (unsigned long long)e->size, nchunks);
}

/**
 * @function on_session_frame: Frame thuộc một phiên (transfer_id != 0).
 * Frame của phiên đã đóng hoặc gửi sai vai bị bỏ qua: chúng có thể là frame
 * đã nằm trên đường truyền trước khi phiên bị huỷ.
 *
 * @param p: Peer gửi.
 * @param h: Header.
 * @param payload: Payload.
 *
 * @return: void
 */
static void on_session_frame(peer_t *p, const frame_hdr_t *h,
                             const uint8_t *payload)
{
    transfer_t *t = transfer_find(h->transfer_id);
    if (!t)
        return;

    int from_src = (p == t->src);
    int from_dst = (p == t->dst);

    switch (h->type) {
    case MSG_CHUNK:
    case MSG_DONE:
        if (!from_src)
            return;
        forward(t->dst, h, payload);
        break;
    case MSG_ACK:
    case MSG_NACK:
    case MSG_REQ_RANGE:
        if (!from_dst)
            return;
        forward(t->src, h, payload);
        break;
    case MSG_CANCEL:
        if (!from_dst)
            return;
        log_msg(TAG, "phiên %u: %s CANCEL", t->id, p->username);
        send_simple(t->src, MSG_ABORT, t->id);
        transfer_close(t);
        return;
    case MSG_OK:
    case MSG_ERR:
        if (from_dst) {
            log_msg(TAG, "phiên %u: kết thúc (%s)", t->id,
                    h->type == MSG_OK ? "OK" : "ERR");
            forward(t->src, h, payload);
            transfer_close(t);
        } else if (from_src && h->type == MSG_ERR) {
            log_msg(TAG, "phiên %u: nguồn báo lỗi, SOURCE_GONE", t->id);
            send_simple(t->dst, MSG_SOURCE_GONE, t->id);
            transfer_close(t);
        }
        return;
    default:
        return;
    }
    t->last_progress_ms = now_ms();
}

int handle_frame(conn_t *c, const frame_hdr_t *h, const uint8_t *payload,
                 void *ctx)
{
    peer_t *p = ctx;
    (void)c;

    if (h->type == MSG_QUIT) {
        on_quit(p);
        return 1;
    }
    if (h->type == MSG_JOIN) {
        on_join(p, payload, h->length);
        return p->closing ? 1 : 0;
    }
    if (!p->username[0]) {
        reply_err(p, E_NOT_JOINED, "cần JOIN trước");
        return p->closing ? 1 : 0;
    }

    switch (h->type) {
    case MSG_SHARE:
        on_share(p, payload, h->length);
        break;
    case MSG_UNSHARE:
        on_unshare(p, payload, h->length);
        break;
    case MSG_LIST:
        on_list(p, NULL);
        break;
    case MSG_SEARCH:
        on_search(p, payload, h->length);
        break;
    case MSG_GET:
        on_get(p, payload, h->length);
        break;
    case MSG_CHUNK:
    case MSG_DONE:
    case MSG_ACK:
    case MSG_NACK:
    case MSG_REQ_RANGE:
    case MSG_CANCEL:
    case MSG_OK:
    case MSG_ERR:
        /* OK/ERR không kèm phiên từ peer không cần trả lời */
        if (h->transfer_id != 0)
            on_session_frame(p, h, payload);
        break;
    default:
        reply_err(p, E_UNKNOWN_TYPE, "loại thông điệp không hợp lệ");
        break;
    }
    return p->closing ? 1 : 0;
}

void handle_disconnect(peer_t *p)
{
    for (size_t i = transfer_count(); i-- > 0;) {
        transfer_t *t = transfer_at(i);
        if (t->src == p) {
            log_msg(TAG, "phiên %u: nguồn mất kết nối, SOURCE_GONE", t->id);
            send_simple(t->dst, MSG_SOURCE_GONE, t->id);
            transfer_close(t);
        } else if (t->dst == p) {
            log_msg(TAG, "phiên %u: người tải mất kết nối, ABORT", t->id);
            send_simple(t->src, MSG_ABORT, t->id);
            transfer_close(t);
        }
    }

    size_t n = p->username[0] ? catalog_remove_owner(p->username) : 0;
    log_msg(TAG, "%s (%s) ngắt kết nối, gỡ %zu file", p->addr,
            p->username[0] ? p->username : "-", n);
}

void handlers_tick(int64_t now)
{
    for (size_t i = transfer_count(); i-- > 0;) {
        transfer_t *t = transfer_at(i);
        if (now - t->last_progress_ms <= NO_PROGRESS_MS)
            continue;
        log_msg(TAG, "phiên %u: không tiến triển quá %d ms, huỷ", t->id,
                NO_PROGRESS_MS);
        send_simple(t->src, MSG_ABORT, t->id);
        send_err(t->dst, t->id, E_NO_PROGRESS, "phiên không tiến triển");
        transfer_close(t);
    }
}
