# BTL Lập trình mạng — Chia sẻ file qua Tracker

Tracker điều phối (relay) + Peer, C/POSIX sockets, một luồng + `poll()` ở cả hai phía.
Giao thức: [docs/protocol.md](docs/protocol.md) · Kế hoạch: [docs/PLAN.md](docs/PLAN.md)

## Build và test

```sh
make                       # bin/tracker, bin/peer, bin/fake_peer, bin/fake_tracker, bin/unit_test
make test                  # unit test + e2e_join + e2e_catalog + e2e_transfer (~10 s)
SLOW=1 tests/e2e_transfer.sh   # thêm case ACK timeout 10 s và NO_PROGRESS 30 s
sudo tests/netem.sh        # mạng xấu bằng tc netem trên loopback
make asan && make test     # chạy toàn bộ dưới AddressSanitizer/UBSan
```

Đặt `KEEP=1` để giữ lại thư mục tạm (log tracker, output từng peer) sau khi test.

## Chạy

```sh
bin/tracker 9000
bin/peer 127.0.0.1 9000 bob
bin/peer 127.0.0.1 9000 alice --dl ./download
```

Lệnh trong Peer:

| Lệnh | Ý nghĩa |
|---|---|
| `share <đường dẫn>` | công bố file (tên công bố = tên file) |
| `unshare <tên>` | ngừng chia sẻ; ai đang tải nhận SOURCE_GONE |
| `list` / `search <từ khoá>` | xem / tìm file đang chia sẻ |
| `get <chủ file> <tên>` | tải về `--dl` (mặc định `download/`), hiện tiến trình |
| `cancel` | huỷ lượt tải đang chạy |
| `status` | file đang chia sẻ, các phiên đang truyền |
| `wait` | chờ yêu cầu và lượt tải xong (dùng trong script) |
| `quit` | thoát |

Tiêm lỗi (mục 4.6): `--fault-drop=3,7,10-12` và `--fault-corrupt=5` làm mất / hỏng **lần gửi đầu**
của các chunk đó khi peer làm nguồn; `--fault-delay=MS` làm chậm mỗi ACK khi peer tải.

Mã thoát của peer: 0 bình thường, 1 lỗi kết nối hoặc JOIN bị từ chối, 2 có lượt tải thất bại.

## Cấu trúc

| Thư mục | Nội dung |
|---|---|
| `common/` | `protocol.h`, frame (header + payload), conn (ráp frame, hàng đợi gửi non-blocking), netio, crc32, sha256, util |
| `tracker/` | `main.c` vòng poll + timeout; `peers` bảng kết nối; `catalog` (owner, name) → size; `transfer` bảng phiên; `handlers` xử lý lệnh và relay |
| `peer/` | `main.c` CLI + hàng chờ phản hồi; `upload` cửa sổ W, gửi lại, DONE; `download` .part, bitmap, ACK/NACK/REQ_RANGE, SHA-256; `shares`; `fault` |
| `tests/` | `unit_test`, `fake_peer` (kịch bản), `fake_tracker`, `lib.sh`, các script e2e, `netem.sh` |

## Tiến độ

- [x] Tuần 1: giao thức, khung dự án, JOIN/QUIT, chương trình giả, unit test
- [x] Tuần 2: SHARE, UNSHARE, LIST (nhiều frame), SEARCH, kiểm tra đầu vào
- [x] Tuần 3: GET, relay, cửa sổ W, ACK, .part, SHA-256, CANCEL/ABORT, BUSY
- [x] Tuần 4: SOURCE_GONE, mất kết nối, CRC32, NACK, REQ_RANGE, ACK timeout, NO_PROGRESS, tiêm lỗi, script netem
- [ ] Tuần 5: báo cáo, demo, mở rộng (P2P, resume)
