# Hàm dùng chung cho các script test (source từ thư mục btl/).

BIN=${BIN:-bin}
PORT=${PORT:-56200}
T=$(mktemp -d)
PASS=0
FAIL=0
TRACKER_PID=
declare -A PEER_FD PEER_PID

cleanup() {
    for n in "${!PEER_PID[@]}"; do kill "${PEER_PID[$n]}" 2>/dev/null; done
    [ -n "$TRACKER_PID" ] && kill "$TRACKER_PID" 2>/dev/null
    wait 2>/dev/null
    [ -z "$KEEP" ] && rm -rf "$T"
}
trap cleanup EXIT

ok()  { echo "PASS  $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL  $1"; FAIL=$((FAIL + 1)); [ -n "$2" ] && sed 's/^/      /' "$2"; }

# check <tên> <lệnh...>: lệnh trả 0 là PASS
check() {
    local name=$1
    shift
    if "$@" >"$T/check_$name.out" 2>&1; then ok "$name"; else bad "$name" "$T/check_$name.out"; fi
}

# run_case <tên> <kịch bản fake_peer>
run_case() {
    local name=$1 script=$2
    if printf '%s\n' "$script" | $BIN/fake_peer 127.0.0.1 "$PORT" "$name" \
        >"$T/$name.out" 2>&1; then ok "$name"; else bad "$name" "$T/$name.out"; fi
}

start_tracker() {
    $BIN/tracker "$PORT" 2>"$T/tracker.log" &
    TRACKER_PID=$!
    sleep 0.3
    if ! kill -0 "$TRACKER_PID" 2>/dev/null; then
        echo "Tracker không chạy được:"
        cat "$T/tracker.log"
        exit 1
    fi
}

# start_peer <username> [tuỳ chọn peer...]: peer đọc lệnh từ FIFO $T/<name>.in,
# in ra $T/<name>.out, tải về $T/dl_<name>/
start_peer() {
    local n=$1 fd
    shift
    mkfifo "$T/$n.in"
    $BIN/peer 127.0.0.1 "$PORT" "$n" --dl "$T/dl_$n" "$@" \
        <"$T/$n.in" >"$T/$n.out" 2>&1 &
    PEER_PID[$n]=$!
    exec {fd}>"$T/$n.in"
    PEER_FD[$n]=$fd
}

# cmd <username> <lệnh>
cmd() { echo "$2" >&"${PEER_FD[$1]}"; }

# stop_peer <username>: gửi quit, đóng FIFO, chờ tiến trình; trả mã thoát
stop_peer() {
    local n=$1 fd=${PEER_FD[$1]} rc
    echo quit >&"$fd" 2>/dev/null
    exec {fd}>&-
    wait "${PEER_PID[$n]}"
    rc=$?
    unset "PEER_PID[$n]" "PEER_FD[$n]"
    return $rc
}

# kill_peer <username>: giết đột ngột (mất kết nối)
kill_peer() {
    local n=$1 fd=${PEER_FD[$1]}
    kill -9 "${PEER_PID[$n]}" 2>/dev/null
    exec {fd}>&-
    wait "${PEER_PID[$n]}" 2>/dev/null
    unset "PEER_PID[$n]" "PEER_FD[$n]"
}

# wait_for <file> <mẫu grep> [giây]: chờ tới khi file chứa mẫu
wait_for() {
    local file=$1 pat=$2 secs=${3:-10} i
    for ((i = 0; i < secs * 10; i++)); do
        grep -q -- "$pat" "$file" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# same_file <a> <b>: so SHA-256
same_file() {
    [ -f "$1" ] && [ -f "$2" ] &&
        [ "$(sha256sum <"$1")" = "$(sha256sum <"$2")" ]
}

# no_part <thư mục>: không còn file .part
no_part() { ! ls "$1"/*.part >/dev/null 2>&1; }

summary() {
    echo "-----"
    echo "$1: $PASS PASS, $FAIL FAIL"
    [ "$FAIL" -eq 0 ]
}
