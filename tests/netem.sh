#!/usr/bin/env bash
# Kiểm thử mạng xấu bằng tc netem trên loopback (cần sudo, kernel có sch_netem).
#   sudo tests/netem.sh                    # mặc định: trễ 50ms±10ms, mất 5%, 20 Mbit
#   NETEM="delay 200ms loss 10%" sudo -E tests/netem.sh
# TCP tự gửi lại gói mất nên tầng ứng dụng không thấy chunk mất: mục tiêu là
# file vẫn đúng, CANCEL/UNSHARE vẫn chạy và không tiến trình nào bị treo.

cd "$(dirname "$0")/.." || exit 1
PORT=${PORT:-56250}
NETEM=${NETEM:-"delay 50ms 10ms loss 5% rate 20mbit"}
. tests/lib.sh

if ! tc qdisc replace dev lo root netem $NETEM; then
    echo "Không bật được netem (cần sudo và module sch_netem)"
    exit 1
fi
trap 'tc qdisc del dev lo root 2>/dev/null; cleanup' EXIT
echo "netem trên lo: $NETEM"

start_tracker
F=$T/files
mkdir -p "$F"
head -c 5000000 /dev/urandom >"$F/a.bin"
head -c 3000000 /dev/urandom >"$F/b.bin"

start_peer src
cmd src "share $F/a.bin"
cmd src "share $F/b.bin"
wait_for "$T/src.out" "Đã chia sẻ b.bin" 30 || { cat "$T/src.out"; exit 1; }

# Tải đúng dưới mạng xấu (giới hạn 120 s để phát hiện treo)
timeout 120 bash -c "printf 'get src a.bin\nwait\nquit\n' |
    $BIN/peer 127.0.0.1 $PORT dl1 --dl $T/dl_d1" >"$T/dl1.out" 2>&1
check netem_sha same_file "$F/a.bin" "$T/dl_d1/a.bin"

# Kết hợp tiêm lỗi + mạng xấu
start_peer fsrc --fault-drop=2,9 --fault-corrupt=4
cmd fsrc "share $F/b.bin"
wait_for "$T/fsrc.out" "Đã chia sẻ" 30
timeout 120 bash -c "printf 'get fsrc b.bin\nwait\nquit\n' |
    $BIN/peer 127.0.0.1 $PORT dl2 --dl $T/dl_d2" >"$T/dl2.out" 2>&1
check netem_fault_sha same_file "$F/b.bin" "$T/dl_d2/b.bin"
check netem_fault_resent wait_for "$T/fsrc.out" "gửi lại 3 chunk" 30
stop_peer fsrc

# CANCEL dưới mạng xấu
start_peer dl3
cmd dl3 "get src a.bin"
wait_for "$T/dl3.out" "bắt đầu tải" 30
sleep 0.5
cmd dl3 cancel
check netem_cancel wait_for "$T/src.out" "bị huỷ (ABORT)" 30
check netem_cancel_no_part no_part "$T/dl_d3"
stop_peer dl3

# UNSHARE giữa chừng dưới mạng xấu
start_peer dl4
cmd dl4 "get src a.bin"
wait_for "$T/dl4.out" "bắt đầu tải" 30
sleep 0.5
cmd src "unshare a.bin"
check netem_source_gone wait_for "$T/dl4.out" "SOURCE_GONE" 30
stop_peer dl4

stop_peer src
check tracker_alive kill -0 "$TRACKER_PID"
summary netem
