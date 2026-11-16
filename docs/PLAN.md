# Plan code BTL — Chia sẻ file qua Tracker (Relay + P2P)

Ngôn ngữ C (gnu11), POSIX sockets, một luồng + `poll()` cho cả Tracker và Peer.
Không dùng thư viện ngoài (tự viết CRC32, SHA-256) để `make` chạy trên mọi máy Linux/WSL.

---

## 0. Cây thư mục

```
btl/
├── Makefile                 # build tất cả: tracker, peer, tests
├── common/
│   ├── protocol.h           # hằng số, mã type, mã lỗi, struct frame_hdr
│   ├── frame.c/.h           # pack/unpack header 22 byte, build payload (put_u8/u32/u64/str)
│   ├── conn.c/.h            # kết nối non-blocking: rbuf ráp frame + sendq có giới hạn
│   ├── netio.c/.h           # tcp_listen, tcp_connect, set_nonblock, now_ms
│   ├── crc32.c/.h
│   ├── sha256.c/.h
│   └── util.c/.h            # valid_filename, valid_username, log
├── tracker/
│   ├── main.c               # parse args, vòng poll()
│   ├── peers.c/.h           # bảng kết nối/peer online
│   ├── catalog.c/.h         # (owner, name) -> size
│   ├── transfer.c/.h        # bảng phiên: transfer_id, src, dst, trạng thái
│   └── handlers.c/.h        # xử lý từng type
├── peer/
│   ├── main.c               # parse args (+ --fault-*), vòng poll(stdin, tracker)
│   ├── cli.c/.h             # đọc & phân tích lệnh người dùng
│   ├── shares.c/.h          # bảng file mình share: name -> path, size
│   ├── upload.c/.h          # phía nguồn: cửa sổ W, retransmit, DONE
│   ├── download.c/.h        # phía nhận: .part, bitmap, ACK/NACK/REQ_RANGE, SHA check
│   └── fault.c/.h           # tiêm lỗi drop/corrupt/delay
├── tests/
│   ├── fake_peer.c          # test Tracker (Thành viên 1)
│   ├── fake_tracker.c       # test Peer (Thành viên 2)
│   ├── unit_test.c          # crc32/sha256 vector chuẩn, frame pack/unpack, valid_*
│   ├── e2e_basic.sh         # 1 tracker + 2 peer, so sha256sum
│   ├── e2e_edge.sh          # CANCEL, UNSHARE giữa chừng, BUSY, kill peer
│   ├── e2e_fault.sh         # --fault-drop/--fault-corrupt
│   └── netem.sh             # tc qdisc add/del dev lo ... (cần sudo)
└── docs/
    ├── protocol.md          # bản "hợp đồng" chi tiết (sinh từ mục 1–2 dưới đây)
    └── PLAN.md
```

`common/` build thành `libcommon.a`, tracker/peer/tests link vào.

---

## 1. `protocol.h` — chốt ngay tuần 1

### 1.1 Hằng số

```c
#define PROTO_MAGIC        0xF5A1
#define PROTO_VERSION      1
#define HDR_SIZE           22
#define MAX_PAYLOAD        (64 * 1024)
#define CHUNK_SIZE         (16 * 1024)
#define WINDOW_W           8
#define MAX_SESSIONS_PER_SRC 2          /* K */
#define MAX_FILE_SIZE      (2ULL << 30) /* 2 GiB */
#define ACK_TIMEOUT_MS     10000
#define MAX_RETRY          3
#define PARTIAL_FRAME_MS   5000
#define NO_PROGRESS_MS     30000
#define SENDQ_LIMIT        (1024 * 1024)
#define FLAG_LAST          0x01         /* bit 0: frame cuối của thông điệp nhiều phần */
```

### 1.2 Mã type

| Hex | Type | Chiều | transfer_id |
|---|---|---|---|
| 0x01 | JOIN | P→T | 0 |
| 0x02 | QUIT | P→T | 0 |
| 0x03 | SHARE | P→T | 0 |
| 0x04 | UNSHARE | P→T | 0 |
| 0x05 | LIST | P→T | 0 |
| 0x06 | SEARCH | P→T | 0 |
| 0x07 | GET | P→T | 0 |
| 0x10 | OK | T→P (hoặc P→T khi kết thúc phiên) | 0 / id |
| 0x11 | ERR | hai chiều | 0 / id |
| 0x12 | LIST_RESP | T→P | 0 |
| 0x20 | SEND_REQ | T→nguồn | id |
| 0x21 | GET_OK | T→người tải | id |
| 0x22 | BUSY | T→người tải | 0 |
| 0x30 | CHUNK | nguồn→T→nhận | id |
| 0x31 | ACK | nhận→T→nguồn | id |
| 0x32 | NACK | nhận→T→nguồn | id |
| 0x33 | REQ_RANGE | nhận→T→nguồn | id |
| 0x34 | DONE | nguồn→T→nhận | id |
| 0x40 | CANCEL | nhận→T | id |
| 0x41 | ABORT | T→nguồn | id |
| 0x42 | SOURCE_GONE | T→nhận | id |

### 1.3 Payload (số nhiều byte đều big-endian; chuỗi = `u8 len` + bytes, không `\0`)

| Type | Payload |
|---|---|
| JOIN | `str username` |
| SHARE | `str name`, `u64 size` |
| UNSHARE | `str name` |
| LIST | rỗng |
| SEARCH | `str keyword` (so khớp chuỗi con, không phân biệt hoa thường) |
| GET | `str owner`, `str name` |
| OK | rỗng (hoặc `str msg`) |
| ERR | `u16 code`, `str msg` |
| LIST_RESP | `u16 count`, rồi `count` × (`str owner`, `str name`, `u64 size`); nhiều frame, frame cuối có `FLAG_LAST` (danh sách rỗng = 1 frame count=0 + LAST) |
| SEND_REQ | `str requester`, `str name`, `u64 size` |
| GET_OK | `str owner`, `str name`, `u64 size`, `u32 nchunks`, `u32 chunk_size` |
| CHUNK | dữ liệu thô; `chunk_id` ở header, `crc32` ở header |
| ACK / NACK | rỗng; `chunk_id` ở header |
| REQ_RANGE | `u32 last_chunk` (chunk đầu ở header `chunk_id`) |
| DONE | `u8 sha256[32]` |
| CANCEL / ABORT / SOURCE_GONE | rỗng (ABORT/SOURCE_GONE có thể kèm `u16 reason`) |

> Tên file ≤ 255 byte nên `u8 len` vừa đủ.

### 1.4 Mã lỗi (ERR)

```
1 E_BAD_FRAME     2 E_NOT_JOINED    3 E_ALREADY_JOINED  4 E_NAME_TAKEN
5 E_BAD_USERNAME  6 E_BAD_FILENAME  7 E_BAD_SIZE        8 E_ALREADY_SHARED
9 E_NOT_FOUND    10 E_OWNER_OFFLINE 11 E_SELF_GET       12 E_TIMEOUT
13 E_NO_PROGRESS 14 E_FILE_IO      15 E_HASH_MISMATCH   16 E_UNKNOWN_TYPE
```

### 1.5 Kết thúc phiên (chỗ đề cương chưa nói rõ — chốt như sau)

1. Nguồn gửi CHUNK theo cửa sổ. Khi **mọi chunk đã gửi ít nhất 1 lần** (không cần chờ ACK hết) thì gửi `DONE(sha256)`.
2. Người nhận: nếu nhận DONE mà bitmap còn thiếu → `REQ_RANGE`/`NACK` cho phần thiếu, chờ tiếp.
3. Đủ chunk **và** đã có DONE → tính SHA-256 file `.part`:
   - khớp → rename, gửi `OK` (transfer_id = id)
   - sai → xoá `.part`, gửi `ERR(E_HASH_MISMATCH)` (transfer_id = id)
4. Tracker nhận OK/ERR có transfer_id ≠ 0 từ người nhận → chuyển tiếp cho nguồn, đóng phiên, giảm bộ đếm K.

Như vậy nguồn cần giữ phiên mở sau khi gửi DONE (vẫn có thể nhận NACK/REQ_RANGE).

---

## 2. Lớp dùng chung (`common/`)

### 2.1 `frame.h`

```c
typedef struct {
    uint16_t magic;
    uint8_t  version, type, flags, reserved;
    uint32_t transfer_id, chunk_id, length, crc32;
} frame_hdr_t;

void hdr_pack(const frame_hdr_t *h, uint8_t out[HDR_SIZE]);
int  hdr_unpack(const uint8_t in[HDR_SIZE], frame_hdr_t *h);  /* -1: magic/version sai, length > MAX_PAYLOAD */

/* Builder payload: ghi tuần tự, báo lỗi nếu tràn */
typedef struct { uint8_t *buf; size_t cap, len; int err; } wbuf_t;
void put_u8/put_u16/put_u32/put_u64/put_str/put_bytes(wbuf_t *, ...);
/* Reader payload: đọc tuần tự, err=1 nếu thiếu byte */
typedef struct { const uint8_t *p; size_t len, pos; int err; } rbuf_t;
uint8_t get_u8(rbuf_t *); ... int get_str(rbuf_t *, char *out, size_t cap);
```

### 2.2 `conn.h` — trái tim của cả hai chương trình

```c
typedef struct {
    int      fd;
    uint8_t  rbuf[HDR_SIZE + MAX_PAYLOAD];
    size_t   rlen;               /* số byte đã ráp của frame hiện tại */
    int64_t  partial_since_ms;   /* thời điểm bắt đầu frame dở; 0 nếu không dở */
    /* sendq: danh sách liên kết các block hoặc ring buffer */
    struct qblk *qhead, *qtail;
    size_t   qbytes;             /* tổng byte chờ gửi */
} conn_t;

int  conn_send_frame(conn_t *c, uint8_t type, uint8_t flags, uint32_t tid,
                     uint32_t cid, const void *payload, uint32_t len); /* tự tính CRC nếu CHUNK; -1 nếu vượt SENDQ_LIMIT */
int  conn_flush(conn_t *c);      /* gọi khi POLLOUT; -1 nếu lỗi */
/* Đọc hết dữ liệu có sẵn, gọi cb cho mỗi frame đủ. Trả -1 nếu EOF/lỗi/frame hỏng */
int  conn_on_readable(conn_t *c, void (*cb)(conn_t *, const frame_hdr_t *, const uint8_t *, void *), void *ctx);
int  conn_wants_write(const conn_t *c);   /* qbytes > 0 → set POLLOUT */
```

Peer cũng dùng `conn_t` non-blocking (không dùng send blocking) để không bao giờ kẹt khi vừa gửi chunk vừa phải nhận lệnh.

### 2.3 CRC32 / SHA-256

- `uint32_t crc32_update(uint32_t crc, const void *, size_t)` — bảng 256 phần tử, đa thức 0xEDB88320.
- `sha256_init/update/final` — kiểm bằng vector "abc" và chuỗi rỗng trong `unit_test.c`, và so với `sha256sum` trên file thật.

---

## 3. Tracker

### 3.1 Cấu trúc dữ liệu

```c
typedef struct peer {
    conn_t   conn;
    char     username[33];   /* rỗng = chưa JOIN */
    int      active_uploads; /* số phiên đang làm nguồn (≤ K) */
    int64_t  last_progress_ms;
    int      closing;        /* đánh dấu để dọn sau vòng poll */
} peer_t;

typedef struct { char owner[33]; char name[256]; uint64_t size; } cat_entry_t;

typedef struct {
    uint32_t id;
    peer_t  *src, *dst;
    char     name[256];
    uint64_t size;
    int64_t  last_progress_ms;
} transfer_t;
```

Quy mô nhỏ → mảng tĩnh (`MAX_PEERS = 128`, `MAX_FILES = 1024`, `MAX_TRANSFERS = 256`) + tìm tuyến tính là đủ, dễ debug.

### 3.2 Vòng lặp chính

```
loop:
  build pollfd[]: listen_fd + mỗi peer (POLLIN, thêm POLLOUT nếu sendq ≠ rỗng)
  timeout = 1000 ms
  poll()
  if listen readable: accept, set_nonblock, thêm peer
  for each peer:
     POLLIN  → conn_on_readable(..., dispatch)
     POLLOUT → conn_flush
     lỗi/EOF → peer_drop(p)
  check_timeouts(now):
     frame dở > 5 s          → ERR(E_TIMEOUT) + drop
     qbytes > 1 MB           → drop
     transfer không tiến triển > 30 s → huỷ phiên (ABORT nguồn, SOURCE_GONE/ERR nhận)
  dọn các peer closing
```

### 3.3 Dispatch (`handlers.c`)

| Lệnh | Kiểm tra | Hành động |
|---|---|---|
| JOIN | chưa join, username hợp lệ, chưa ai dùng | lưu username, OK |
| SHARE | đã join, tên hợp lệ, 0 < size ≤ 2 GiB, chưa share trùng | thêm catalog, OK |
| UNSHARE | có entry | xoá entry; mọi phiên của (owner,name): SOURCE_GONE cho dst, ABORT cho src; OK |
| LIST / SEARCH | đã join | duyệt catalog, chia frame ≤ MAX_PAYLOAD, frame cuối FLAG_LAST |
| GET | 5 điều kiện ở mục 5 đề cương | `active_uploads ≥ K` → BUSY; ngược lại tạo transfer, SEND_REQ→src, GET_OK→dst |
| CHUNK, DONE | sender == t->src | forward nguyên frame cho t->dst, cập nhật last_progress |
| ACK, NACK, REQ_RANGE | sender == t->dst | forward cho t->src, cập nhật last_progress |
| OK/ERR có tid | sender == t->dst | forward cho src, đóng phiên |
| ERR có tid từ src | (nguồn không mở được file) | SOURCE_GONE cho dst, đóng phiên |
| CANCEL | sender == t->dst | ABORT→src, đóng phiên |
| QUIT | — | như mất kết nối |
| type lạ | — | ERR(E_UNKNOWN_TYPE) |

`peer_drop(p)`: với mỗi phiên p là src → SOURCE_GONE cho dst; p là dst → ABORT cho src; xoá các file của p khỏi catalog; close fd.

Tracker **không** kiểm CRC khi relay (đó là việc của người nhận) — chỉ kiểm header hợp lệ.

---

## 4. Peer

### 4.1 Dòng lệnh

```
./peer <tracker_host> <port> <username> [--dir ./shared] [--dl ./download]
       [--fault-drop=3,7] [--fault-corrupt=5,12] [--fault-delay=ms]
```

Lệnh người dùng: `share <path>`, `unshare <name>`, `list`, `search <kw>`, `get <owner> <name>`, `cancel`, `status`, `quit`, `help`.

### 4.2 Vòng lặp

```
poll({stdin, tracker_fd}, timeout = 200 ms)
  stdin  → cli_handle_line()
  tracker POLLIN  → dispatch frame
  tracker POLLOUT → conn_flush
  tick(now): upload_tick (gửi thêm chunk nếu cửa sổ còn chỗ, retransmit khi quá ACK_TIMEOUT)
             download_tick (in tiến trình)
```

Mỗi lần `upload_tick` chỉ đẩy chunk khi `conn.qbytes` còn nhỏ (vd < 256 KB) để không tự làm đầy sendq.

### 4.3 Upload (phía nguồn) — tối đa K phiên

```c
typedef struct {
    uint32_t tid; int fd; uint64_t size; uint32_t nchunks;
    uint32_t next_new;            /* chunk mới chưa gửi lần nào */
    uint8_t *acked;               /* bitmap */
    int64_t *sent_at;             /* thời điểm gửi gần nhất (theo chunk trong cửa sổ) */
    uint8_t *retries;
    uint32_t inflight;            /* đã gửi, chưa ACK */
    uint32_t *resend_q; size_t resend_n; /* chunk bị NACK/REQ_RANGE chờ gửi lại */
    int done_sent; uint8_t sha[32];
} upload_t;
```

- SEND_REQ: mở file (kiểm size khớp), tính SHA-256 trước (hoặc tính dần khi gửi lần đầu theo thứ tự), tạo upload_t. Mở lỗi → ERR(tid, E_FILE_IO).
- Gửi: ưu tiên `resend_q`, rồi chunk mới nếu `inflight < W`. Đọc bằng `pread(fd, buf, len, cid*CHUNK_SIZE)`.
- ACK: set bit, inflight--. NACK/REQ_RANGE: đẩy các cid vào resend_q.
- Timeout chunk > 10 s → gửi lại, `retries++`; > 3 → ERR(tid, E_TIMEOUT), huỷ.
- `next_new == nchunks && !done_sent` → gửi DONE.
- ABORT / OK / ERR(tid) → đóng file, giải phóng.

### 4.4 Download (phía nhận) — 1 phiên tại một thời điểm (đủ cho BTL)

- GET_OK: kiểm tên (`valid_filename`), tạo `download/<name>.part` (`O_CREAT|O_TRUNC`), `ftruncate` không bắt buộc. Bitmap `nchunks` bit.
- CHUNK: kiểm `tid`, `cid < nchunks`, `length` đúng (CHUNK_SIZE, hoặc phần dư với chunk cuối), CRC32 khớp.
  - sai → NACK(cid)
  - đúng → `pwrite` tại `cid*CHUNK_SIZE`; lỗi ghi → NACK; ok → set bit, ACK(cid)
  - **phát hiện bỏ sót**: TCP giữ thứ tự, nên nếu nhận `cid > expected_next` (expected = chunk mới lớn nhất + 1) mà khoảng giữa chưa có → REQ_RANGE(expected, cid-1).
  - đếm byte: tổng vượt `size` → CANCEL + xoá.
- DONE: lưu sha; nếu thiếu → REQ_RANGE cho từng khoảng trống.
- Đủ + có DONE → SHA-256 file `.part` → rename sang tên không trùng (`name`, `name (1)`, ...) → OK(tid); sai → xoá, ERR(tid, E_HASH_MISMATCH).
- SOURCE_GONE / ERR(tid) / `cancel` → xoá `.part`, báo người dùng (`cancel` gửi CANCEL trước).
- Tiến trình: `\r[#####.....] 52% 12.3 MB/s` cập nhật mỗi ≥ 200 ms.

### 4.5 Fault injection (`fault.c`)

- `--fault-drop=3,7`: lần gửi **đầu tiên** của chunk 3, 7 bị bỏ (coi như đã gửi). Lần gửi lại phải đi thật → chứng minh chỉ gửi lại đúng chunk đó.
- `--fault-corrupt=5`: lần gửi đầu chunk 5 lật 1 byte **sau khi** tính CRC → bên nhận NACK.
- `--fault-delay=ms`: phía nhận trì hoãn gửi ACK (đặt vào hàng đợi có timestamp).
- Peer in log `[fault] drop chunk 3`, `[resend] chunk 3 (NACK)` để demo/đếm trong test script.

---

## 5. Chương trình giả để test độc lập

- **fake_peer** (Thành viên 1): đọc kịch bản từ stdin, mỗi dòng 1 lệnh: `JOIN alice`, `SHARE a.txt 1000`, `GET bob a.txt`, `RAW badmagic`, `RAW huge_length`, `HALF_FRAME` (gửi nửa header rồi ngủ), `SLOW_READ` (không đọc socket), `EXPECT OK` ... In các frame nhận được. Dùng để test JOIN trùng tên, frame hỏng, 5 s timeout, 1 MB sendq, BUSY.
- **fake_tracker** (Thành viên 2): accept 1–2 peer, kịch bản cứng:
  - chế độ `serve-upload`: gửi SEND_REQ cho peer, nhận chunk, kiểm CRC/sha, thử gửi NACK/REQ_RANGE/ABORT.
  - chế độ `serve-download`: gửi GET_OK + chunk từ một file local, cố tình bỏ/hỏng chunk, gửi SOURCE_GONE giữa chừng.

---

## 6. Lộ trình theo tuần (gắn với phân công)

### Tuần 1 — Giao thức & khung (cả hai)
- [x] Chốt `protocol.h` + `docs/protocol.md` (mục 1)
- [x] `frame.c`, `conn.c`, `netio.c`, `util.c` + `unit_test.c` (pack/unpack, valid_*)
- [x] Makefile (`make`, `make test`, `make clean`, `-Wall -Wextra -g`, thêm target `asan`)
- [x] TV1: tracker accept + poll + JOIN/QUIT; fake_peer tối thiểu
- [x] TV2: peer kết nối + CLI + JOIN/QUIT; fake_tracker tối thiểu
- **Xong khi**: 3 peer JOIN cùng lúc, tên trùng nhận ERR, gửi magic sai bị đóng kết nối.

### Tuần 2 — Catalog & nhiều client
- [x] TV1: catalog, SHARE/UNSHARE/LIST/SEARCH (LIST nhiều frame), kiểm tra đầu vào, timeout frame dở, giới hạn sendq
- [x] TV2: `share/unshare/list/search` ở peer, bảng shares, in danh sách đẹp
- [x] crc32.c, sha256.c + test vector
- **Xong khi**: share 2000 file → LIST trả về đủ qua nhiều frame; peer thoát → file biến khỏi LIST.

### Tuần 3 — Truyền file
- [x] TV1: GET (5 điều kiện), transfer table, relay CHUNK/ACK/DONE/OK, CANCEL→ABORT, BUSY (K=2)
- [x] TV2: upload (cửa sổ W, pread), download (.part, pwrite, bitmap, ACK, SHA, rename không ghi đè), cancel, tiến trình
- **Xong khi**: `e2e_basic.sh` tải file 1 GB, `sha256sum` khớp; RSS của tracker giữ thấp (`/proc/<pid>/status`).

### Tuần 4 — Tình huống biên & mạng yếu
- [x] TV1: SOURCE_GONE (UNSHARE/disconnect), receiver disconnect → ABORT, người nhận chậm (30 s / 1 MB), fake_peer `SLOW_READ`
- [x] TV2: CRC check + NACK, phát hiện bỏ sót + REQ_RANGE, ACK timeout + retry 3 lần, fault-drop/corrupt/delay
- [x] Cả hai: `e2e_edge.sh`, `e2e_fault.sh`, `netem.sh` (delay 200ms, loss 10%, rate 1mbit trên `lo`)
- **Xong khi**: mọi case ở mục 4.3 và 4.6 đề cương có script chạy được, in PASS/FAIL.

### Tuần 5 — Hoàn thiện
- [ ] README (build, chạy, lệnh), báo cáo, kịch bản demo
- [ ] Chạy `make asan` + valgrind trên toàn bộ test
- [ ] Mở rộng nếu còn thời gian: P2P (peer mở cổng listen, GET_OK kèm IP:port, fallback relay) → resume (giữ `.part` + bitmap file `.bitmap`)

---

## 7. Checklist chống lỗi hay gặp

- `signal(SIGPIPE, SIG_IGN)` + `send(..., MSG_NOSIGNAL)` ở cả hai bên.
- Mọi `recv`/`send` non-blocking xử lý `EAGAIN`/`EINTR`; `recv == 0` là EOF.
- Không `free(peer)` giữa vòng duyệt pollfd — đánh dấu `closing` rồi dọn cuối vòng.
- Khi drop peer phải dọn **mọi** phiên liên quan trước khi xoá peer (tránh con trỏ treo trong transfer_t).
- `length` kiểm tra trước khi đọc payload; chuỗi `get_str` luôn kiểm `len < cap`.
- Tên file nhận từ mạng phải `valid_filename` lại ở peer trước khi ghép đường dẫn.
- Dùng `CLOCK_MONOTONIC` cho mọi timeout.
- Bitmap/sent_at cấp phát theo `nchunks` (tối đa 131072 với 2 GiB) — kiểm `malloc` NULL.
