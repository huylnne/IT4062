#!/usr/bin/env bash
# Test tuần 2 (chỉ dùng fake_peer): SHARE, UNSHARE, LIST, SEARCH, kiểm tra
# đầu vào, GET và SOURCE_GONE ở mức giao thức.

cd "$(dirname "$0")/.." || exit 1
PORT=${PORT:-56210}
. tests/lib.sh
start_tracker

run_case share_basic "JOIN anna
EXPECT OK
SHARE report.pdf 1000
EXPECT OK
SHARE report.pdf 1000
EXPECT ERR ALREADY_SHARED
SHARE ../evil 10
EXPECT ERR BAD_FILENAME
SHARE .. 10
EXPECT ERR BAD_FILENAME
SHARE zero.bin 0
EXPECT ERR BAD_SIZE
SHARE huge.bin 3000000000
EXPECT ERR BAD_SIZE
UNSHARE nope.txt
EXPECT ERR NOT_FOUND
LIST
EXPECT_LIST 1
UNSHARE report.pdf
EXPECT OK
LIST
EXPECT_LIST 0"

# Hai người chia sẻ cùng tên không đụng nhau; SEARCH không phân biệt hoa thường
printf 'JOIN owner1\nEXPECT OK\nSHARE Movie.mkv 500\nEXPECT OK\nSHARE notes.txt 20\nEXPECT OK\nSLEEP 1500\nQUIT\nEXPECT OK\n' |
    $BIN/fake_peer 127.0.0.1 "$PORT" owner1 >"$T/owner1.out" 2>&1 &
H1=$!
sleep 0.3
run_case same_name_search "JOIN owner2
EXPECT OK
SHARE Movie.mkv 700
EXPECT OK
LIST
EXPECT_LIST 3
SEARCH movie
EXPECT_LIST 2
SEARCH NOTES
EXPECT_LIST 1
SEARCH zzz
EXPECT_LIST 0
GET owner2 Movie.mkv
EXPECT ERR SELF_GET
GET ghost Movie.mkv
EXPECT ERR OWNER_OFFLINE
GET owner1 missing.txt
EXPECT ERR NOT_FOUND"
wait $H1

# Peer thoát thì file của họ biến khỏi catalog
run_case files_gone_on_quit "JOIN viewer
EXPECT OK
LIST
EXPECT_LIST 0"

# 2000 file tên dài -> LIST phải chia nhiều frame
NAME_PAD=$(printf 'x%.0s' {1..100})
{
    echo "JOIN bulk"
    echo "EXPECT OK"
    for i in $(seq 1 2000); do echo "SHARE ${NAME_PAD}_$i.dat $i"; done
    for i in $(seq 1 2000); do echo "EXPECT OK"; done
    echo "SLEEP 1500"
} | $BIN/fake_peer 127.0.0.1 "$PORT" bulk >"$T/bulk.out" 2>&1 &
H2=$!
sleep 1
run_case list_2000_multiframe "JOIN lister
EXPECT OK
LIST
EXPECT_LIST 2000
SEARCH _1999.
EXPECT_LIST 1"
wait $H2
check bulk_share_ok test "$(grep -c PASS "$T/bulk.out")" -eq 2001

# GET mở phiên: nguồn nhận SEND_REQ, người tải nhận GET_OK; nguồn thoát
# giữa chừng thì người tải nhận SOURCE_GONE
printf 'JOIN src1\nEXPECT OK\nSHARE data.bin 100000\nEXPECT OK\nTIMEOUT 5000\nEXPECT SEND_REQ\nSLEEP 300\nQUIT\nEXPECT OK\n' |
    $BIN/fake_peer 127.0.0.1 "$PORT" src1 >"$T/src1.out" 2>&1 &
H3=$!
sleep 0.3
run_case get_then_source_gone "JOIN dst1
EXPECT OK
GET src1 data.bin
EXPECT GET_OK
TIMEOUT 5000
EXPECT SOURCE_GONE"
wait $H3
check src_got_send_req test $? -eq 0

# Người tải huỷ: nguồn nhận ABORT
printf 'JOIN src2\nEXPECT OK\nSHARE data.bin 100000\nEXPECT OK\nTIMEOUT 5000\nEXPECT SEND_REQ\nEXPECT ABORT\n' |
    $BIN/fake_peer 127.0.0.1 "$PORT" src2 >"$T/src2.out" 2>&1 &
H4=$!
sleep 0.3
run_case cancel_sends_abort "JOIN dst2
EXPECT OK
GET src2 data.bin
EXPECT GET_OK
SLEEP 200
CANCEL"
wait $H4
check src_got_abort test $? -eq 0

# Giới hạn K = 2 phiên mỗi nguồn: lượt thứ 3 nhận BUSY
printf 'JOIN src3\nEXPECT OK\nSHARE f.bin 100000\nEXPECT OK\nSLEEP 2000\n' |
    $BIN/fake_peer 127.0.0.1 "$PORT" src3 >"$T/src3.out" 2>&1 &
H5=$!
sleep 0.3
run_case busy_after_k "JOIN dst3
EXPECT OK
GET src3 f.bin
EXPECT GET_OK
GET src3 f.bin
EXPECT GET_OK
GET src3 f.bin
EXPECT BUSY"
wait $H5

check tracker_alive kill -0 "$TRACKER_PID"
summary e2e_catalog
