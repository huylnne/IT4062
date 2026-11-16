# Giao thức Tracker ↔ Peer (phiên bản 1)

Bản "hợp đồng" chung của hai thành viên. Mã nguồn tương ứng: `common/protocol.h`, `common/frame.h`.
Sửa giao thức thì sửa **cả hai** file này trong cùng một commit.

## 1. Frame

Mọi thông điệp = header 22 byte + payload `length` byte. Số nhiều byte theo network byte order (big-endian).

| Offset | Trường | Kiểu | Ghi chú |
|---|---|---|---|
| 0 | magic | u16 | `0xF5A1` |
| 2 | version | u8 | `1` |
| 3 | type | u8 | mục 2 |
| 4 | flags | u8 | bit 0 = `FLAG_LAST` |
| 5 | reserved | u8 | `0` |
| 6 | transfer_id | u32 | 0 nếu không thuộc phiên |
| 10 | chunk_id | u32 | số thứ tự chunk / chunk đầu của REQ_RANGE |
| 14 | length | u32 | ≤ `MAX_PAYLOAD` = 65536 |
| 18 | crc32 | u32 | CRC32 (IEEE) của payload; bên gửi luôn điền, bên nhận chỉ bắt buộc kiểm với CHUNK |

Magic sai, version sai, `length > MAX_PAYLOAD` hoặc frame đọc dở quá 5 s → Tracker gửi ERR rồi đóng kết nối.
Tracker chuyển tiếp frame của phiên **nguyên vẹn** (giữ crc32 gốc).

## 2. Kiểu dữ liệu trong payload

- `u8/u16/u32/u64`: big-endian.
- `str`: `u8 len` + `len` byte, không có `\0`, không được chứa `\0`.
- Payload phải đúng khuôn: thiếu hoặc thừa byte đều là lỗi.

## 3. Các loại thông điệp

| Hex | Type | Chiều | Payload |
|---|---|---|---|
| 01 | JOIN | P→T | `str username` |
| 02 | QUIT | P→T | — |
| 03 | SHARE | P→T | `str name`, `u64 size` |
| 04 | UNSHARE | P→T | `str name` |
| 05 | LIST | P→T | — |
| 06 | SEARCH | P→T | `str keyword` (chuỗi con, không phân biệt hoa thường) |
| 07 | GET | P→T | `str owner`, `str name` |
| 10 | OK | T→P; nhận→T khi kết thúc phiên | — |
| 11 | ERR | hai chiều | `u16 code`, `str msg` |
| 12 | LIST_RESP | T→P | `u16 count`, `count` × (`str owner`, `str name`, `u64 size`); nhiều frame, frame cuối có `FLAG_LAST` |
| 20 | SEND_REQ | T→nguồn | `str requester`, `str name`, `u64 size` |
| 21 | GET_OK | T→nhận | `str owner`, `str name`, `u64 size`, `u32 nchunks`, `u32 chunk_size` |
| 22 | BUSY | T→nhận | — |
| 30 | CHUNK | nguồn→T→nhận | dữ liệu thô của chunk `chunk_id` |
| 31 | ACK | nhận→T→nguồn | — (`chunk_id` ở header) |
| 32 | NACK | nhận→T→nguồn | — (`chunk_id` ở header) |
| 33 | REQ_RANGE | nhận→T→nguồn | `u32 last_chunk` (chunk đầu ở header) |
| 34 | DONE | nguồn→T→nhận | `u8 sha256[32]` |
| 40 | CANCEL | nhận→T | — |
| 41 | ABORT | T→nguồn | — |
| 42 | SOURCE_GONE | T→nhận | — |

Các frame có `transfer_id ≠ 0`: SEND_REQ, GET_OK, CHUNK, ACK, NACK, REQ_RANGE, DONE, CANCEL, ABORT, SOURCE_GONE và OK/ERR kết thúc phiên.

## 4. Mã lỗi

| Mã | Tên | Khi nào |
|---|---|---|
| 1 | BAD_FRAME | header hỏng, payload sai khuôn |
| 2 | NOT_JOINED | gửi lệnh trước JOIN |
| 3 | ALREADY_JOINED | JOIN lần hai |
| 4 | NAME_TAKEN | username đang online |
| 5 | BAD_USERNAME | không phải 3–32 ký tự `[A-Za-z0-9_]` |
| 6 | BAD_FILENAME | rỗng, > 255 byte, có `/` `\`, là `.` hoặc `..` |
| 7 | BAD_SIZE | size = 0 hoặc > 2 GiB |
| 8 | ALREADY_SHARED | (owner, name) đã có trong catalog |
| 9 | NOT_FOUND | không có file trong catalog |
| 10 | OWNER_OFFLINE | chủ file không online |
| 11 | SELF_GET | tải file của chính mình |
| 12 | TIMEOUT | frame dở > 5 s, chờ ACK quá số lần thử |
| 13 | NO_PROGRESS | phiên không tiến triển > 30 s |
| 14 | FILE_IO | nguồn không đọc được file / nhận không ghi được |
| 15 | HASH_MISMATCH | SHA-256 file nhận khác DONE |
| 16 | UNKNOWN_TYPE | type lạ hoặc chưa hỗ trợ |
| 17 | FULL | catalog, bảng phiên hoặc bảng peer đã đầy |

## 5. Hằng số

| Tên | Giá trị |
|---|---|
| MAX_PAYLOAD | 64 KB |
| CHUNK_SIZE | 16 KB |
| WINDOW_W | 8 chunk chưa ACK |
| MAX_SESSIONS_PER_SRC (K) | 2 |
| MAX_FILE_SIZE | 2 GiB |
| ACK_TIMEOUT | 10 s, thử lại tối đa 3 lần |
| PARTIAL_FRAME | 5 s |
| NO_PROGRESS | 30 s |
| SENDQ_LIMIT | 1 MB mỗi kết nối |

## 6. Phiên truyền

```
A: GET(owner=B, name)            → T
T: kiểm tra; quá K phiên → BUSY  → A
T: SEND_REQ(tid)                 → B
T: GET_OK(tid, size, nchunks)    → A
B: CHUNK(tid, i) ... (≤ W chưa ACK) → T → A
A: ACK(tid, i) / NACK(tid, i) / REQ_RANGE(tid, a..b) → T → B
B: DONE(tid, sha256) khi mọi chunk đã gửi ít nhất một lần → T → A
A: còn thiếu → REQ_RANGE; đủ → kiểm SHA-256
A: OK(tid) hoặc ERR(tid, HASH_MISMATCH) → T → B;  T đóng phiên
```

Huỷ / lỗi:

- A gửi CANCEL(tid) → T gửi ABORT(tid) cho B, đóng phiên.
- B UNSHARE / mất kết nối / gửi ERR(tid) → T gửi SOURCE_GONE(tid) cho A, đóng phiên.
- A mất kết nối → T gửi ABORT(tid) cho B.
- Phiên không tiến triển 30 s → T gửi ABORT cho B và ERR(tid, NO_PROGRESS) cho A.
