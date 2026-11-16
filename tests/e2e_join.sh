#!/usr/bin/env bash
# Test tuần 1: JOIN/QUIT, kiểm tra username, frame hỏng, frame dở, nhiều client.
# Chạy từ thư mục btl/:  tests/e2e_join.sh   (cổng đổi bằng PORT=...)

cd "$(dirname "$0")/.." || exit 1
PORT=${PORT:-56200}
BIN=bin
LOG=$(mktemp -d)
PASS=0
FAIL=0

cleanup() {
    [ -n "$TRACKER_PID" ] && kill "$TRACKER_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$LOG"
}
trap cleanup EXIT

# run_case <tên> <kịch bản>: chạy fake_peer với kịch bản, đếm PASS/FAIL
run_case() {
    local name=$1 script=$2
    if printf '%s\n' "$script" | $BIN/fake_peer 127.0.0.1 "$PORT" "$name" \
        >"$LOG/$name.out" 2>&1; then
        echo "PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL  $name"
        sed 's/^/      /' "$LOG/$name.out"
        FAIL=$((FAIL + 1))
    fi
}

# check <tên> <lệnh...>: lệnh trả 0 là PASS
check() {
    local name=$1
    shift
    if "$@" >"$LOG/$name.out" 2>&1; then
        echo "PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL  $name"
        sed 's/^/      /' "$LOG/$name.out"
        FAIL=$((FAIL + 1))
    fi
}

$BIN/tracker "$PORT" 2>"$LOG/tracker.log" &
TRACKER_PID=$!
sleep 0.3
if ! kill -0 "$TRACKER_PID" 2>/dev/null; then
    echo "Tracker không chạy được:"
    cat "$LOG/tracker.log"
    exit 1
fi

run_case join_quit "JOIN alice
EXPECT OK
QUIT
EXPECT OK
EXPECT_CLOSE"

run_case bad_username "JOIN ab
EXPECT ERR BAD_USERNAME
JOIN bad-name
EXPECT ERR BAD_USERNAME"

run_case not_joined "LIST
EXPECT ERR NOT_JOINED"

run_case join_twice "JOIN carol
EXPECT OK
JOIN carol2
EXPECT ERR ALREADY_JOINED"

run_case unknown_type "JOIN dave
EXPECT OK
RAW 0x7F
EXPECT ERR UNKNOWN_TYPE"

run_case bad_magic "BADMAGIC
EXPECT ERR BAD_FRAME
EXPECT_CLOSE"

run_case huge_length "HUGE
EXPECT ERR BAD_FRAME
EXPECT_CLOSE"

# Trùng tên: A giữ tên trong 1.5 s, B thử JOIN cùng tên ở giữa khoảng đó
printf 'JOIN bob\nEXPECT OK\nSLEEP 1500\nQUIT\nEXPECT OK\n' |
    $BIN/fake_peer 127.0.0.1 "$PORT" holder >"$LOG/holder.out" 2>&1 &
HOLDER=$!
run_case name_taken "SLEEP 500
JOIN bob
EXPECT ERR NAME_TAKEN"
wait "$HOLDER"
check name_holder test $? -eq 0

# Sau khi A thoát, tên được giải phóng
run_case name_freed "JOIN bob
EXPECT OK"

# Ba client cùng lúc
pids=()
for u in u_one u_two u_three; do
    printf 'JOIN %s\nEXPECT OK\nSLEEP 500\nQUIT\nEXPECT OK\n' "$u" |
        $BIN/fake_peer 127.0.0.1 "$PORT" "$u" >"$LOG/$u.out" 2>&1 &
    pids+=($!)
done
ok=0
for p in "${pids[@]}"; do wait "$p" || ok=1; done
check three_clients test $ok -eq 0

# Frame dở > 5 s: Tracker gửi ERR TIMEOUT rồi đóng
run_case half_frame "HALF
TIMEOUT 8000
EXPECT ERR TIMEOUT
EXPECT_CLOSE"

# Peer thật: JOIN, help, quit
check peer_join_quit bash -c "printf 'help\nquit\n' | $BIN/peer 127.0.0.1 $PORT real_peer | grep -q 'Đã vào hệ thống'"

# Peer thật bị từ chối tên trùng thì thoát mã 1
printf 'JOIN taken_x\nEXPECT OK\nSLEEP 1000\n' |
    $BIN/fake_peer 127.0.0.1 "$PORT" holder2 >/dev/null 2>&1 &
HOLDER=$!
sleep 0.3
check peer_name_taken bash -c "! printf 'quit\n' | $BIN/peer 127.0.0.1 $PORT taken_x"
wait "$HOLDER"

check tracker_alive kill -0 "$TRACKER_PID"

echo "-----"
echo "e2e_join: $PASS PASS, $FAIL FAIL"
[ "$FAIL" -eq 0 ]
