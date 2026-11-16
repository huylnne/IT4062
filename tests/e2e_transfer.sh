#!/usr/bin/env bash
# Test tuần 3-4 với Peer thật: truyền file, tiêm lỗi (NACK, REQ_RANGE),
# BUSY, CANCEL, UNSHARE giữa chừng, nguồn/người nhận mất kết nối.
# SLOW=1 chạy thêm các case chờ timeout (ACK_TIMEOUT 10 s, NO_PROGRESS 30 s).

cd "$(dirname "$0")/.." || exit 1
PORT=${PORT:-56220}
. tests/lib.sh
start_tracker

F=$T/files
mkdir -p "$F"
head -c 1 /dev/urandom >"$F/one.bin"
head -c 16384 /dev/urandom >"$F/exact.bin"
head -c 16385 /dev/urandom >"$F/plus1.bin"
head -c 20000017 /dev/urandom >"$F/big.bin"
head -c 2000000 /dev/urandom >"$F/fault.bin"
head -c 8000000 /dev/urandom >"$F/slow.bin"
head -c 40000 /dev/urandom >"$F/tiny.bin"
head -c 131073 /dev/urandom >"$F/win.bin"   # 9 chunk: vượt cửa sổ W = 8

# ---- Nguồn chính ----
start_peer src
for f in one exact plus1 big slow tiny win; do cmd src "share $F/$f.bin"; done
wait_for "$T/src.out" "Đã chia sẻ win.bin" || { cat "$T/src.out"; exit 1; }

# ---- 1. Tải đúng và đủ với nhiều kích thước ----
printf 'get src one.bin\nwait\nget src exact.bin\nwait\nget src plus1.bin\nwait\nget src big.bin\nwait\nquit\n' |
    $BIN/peer 127.0.0.1 "$PORT" dl1 --dl "$T/dl_dl1" >"$T/dl1.out" 2>&1
check dl1_exit test $? -eq 0
for f in one exact plus1 big; do
    check "sha_$f" same_file "$F/$f.bin" "$T/dl_dl1/$f.bin"
done
check dl1_no_part no_part "$T/dl_dl1"

# ---- 2. Không ghi đè: tải lại thì thêm hậu tố (1) ----
printf 'get src one.bin\nwait\nquit\n' |
    $BIN/peer 127.0.0.1 "$PORT" dl1 --dl "$T/dl_dl1" >"$T/dl1b.out" 2>&1
check no_overwrite same_file "$F/one.bin" "$T/dl_dl1/one (1).bin"

# ---- 3. Hai người tải cùng lúc ----
printf 'get src big.bin\nwait\nquit\n' |
    $BIN/peer 127.0.0.1 "$PORT" par_a --dl "$T/dl_par_a" >"$T/par_a.out" 2>&1 &
PA=$!
printf 'get src big.bin\nwait\nquit\n' |
    $BIN/peer 127.0.0.1 "$PORT" par_b --dl "$T/dl_par_b" >"$T/par_b.out" 2>&1 &
PB=$!
wait $PA; ra=$?
wait $PB; rb=$?
check parallel_exit test $ra -eq 0 -a $rb -eq 0
check parallel_sha_a same_file "$F/big.bin" "$T/dl_par_a/big.bin"
check parallel_sha_b same_file "$F/big.bin" "$T/dl_par_b/big.bin"

# ---- 4. Tiêm lỗi: chỉ đúng các chunk lỗi được gửi lại ----
# fault.bin = 2000000 B = 123 chunk (0..122): bỏ 3, 7, 122 (chunk cuối), làm hỏng 5
start_peer fsrc --fault-drop=3,7,122 --fault-corrupt=5
cmd fsrc "share $F/fault.bin"
wait_for "$T/fsrc.out" "Đã chia sẻ fault.bin"
printf 'get fsrc fault.bin\nwait\nquit\n' |
    $BIN/peer 127.0.0.1 "$PORT" fdl --dl "$T/dl_fdl" >"$T/fdl.out" 2>&1
check fault_sha same_file "$F/fault.bin" "$T/dl_fdl/fault.bin"
check fault_receiver_stats grep -q "NACK 1, REQ_RANGE 3, chunk trùng 0" "$T/fdl.out"
check fault_nack_chunk5 grep -q "chunk 5 sai CRC32, gửi NACK" "$T/fdl.out"
check fault_range_3 grep -q "thiếu chunk 3-3" "$T/fdl.out"
check fault_range_7 grep -q "thiếu chunk 7-7" "$T/fdl.out"
check fault_range_last grep -q "thiếu chunk 122-122" "$T/fdl.out"
wait_for "$T/fsrc.out" "đầy đủ"
check fault_only_4_resent grep -q "gửi lại 4 chunk" "$T/fsrc.out"
stop_peer fsrc

# ---- 5. BUSY khi nguồn đã phục vụ K = 2 phiên; rồi CANCEL ----
# --fault-delay làm chậm ACK nên lượt tải kéo dài vài giây
start_peer slow1 --fault-delay=100
start_peer slow2 --fault-delay=100
cmd slow1 "get src slow.bin"
cmd slow2 "get src slow.bin"
wait_for "$T/slow1.out" "bắt đầu tải" && wait_for "$T/slow2.out" "bắt đầu tải"
printf 'get src tiny.bin\nwait\nquit\n' |
    $BIN/peer 127.0.0.1 "$PORT" third --dl "$T/dl_third" >"$T/third.out" 2>&1
check busy grep -q "BUSY" "$T/third.out"

cmd slow1 cancel
check cancel_receiver wait_for "$T/slow1.out" "người dùng huỷ"
check cancel_source_abort wait_for "$T/src.out" "bị huỷ (ABORT)"
check cancel_no_part no_part "$T/dl_slow1"
check cancel_tracker_log grep -q "CANCEL" "$T/tracker.log"

# Hết bận: người khác tải được ngay
printf 'get src tiny.bin\nwait\nquit\n' |
    $BIN/peer 127.0.0.1 "$PORT" third --dl "$T/dl_third" >"$T/third2.out" 2>&1
check after_cancel_ok same_file "$F/tiny.bin" "$T/dl_third/tiny.bin"

# ---- 6. Nguồn UNSHARE giữa chừng -> SOURCE_GONE ----
cmd src "unshare slow.bin"
check unshare_source_gone wait_for "$T/slow2.out" "SOURCE_GONE"
check unshare_no_part no_part "$T/dl_slow2"
stop_peer slow2
check unshare_exit_code test $? -eq 2
printf 'search slow\nquit\n' | $BIN/peer 127.0.0.1 "$PORT" lookr >"$T/lookr.out" 2>&1
check unshare_gone_from_catalog grep -q "(0 file)" "$T/lookr.out"
stop_peer slow1

# ---- 7. Nguồn mất kết nối đột ngột ----
start_peer crash_src
cmd crash_src "share $F/slow.bin"
wait_for "$T/crash_src.out" "Đã chia sẻ"
start_peer victim --fault-delay=100
cmd victim "get crash_src slow.bin"
wait_for "$T/victim.out" "bắt đầu tải"
sleep 0.5
kill_peer crash_src
check source_crash_gone wait_for "$T/victim.out" "SOURCE_GONE"
check source_crash_no_part no_part "$T/dl_victim"
check source_crash_catalog wait_for "$T/tracker.log" "(crash_src) ngắt kết nối, gỡ 1 file"
stop_peer victim

# ---- 8. Người nhận mất kết nối đột ngột -> ABORT cho nguồn ----
start_peer crash_dl --fault-delay=100
cmd crash_dl "get src big.bin"
wait_for "$T/crash_dl.out" "bắt đầu tải"
sleep 0.5
before=$(grep -c "bị huỷ (ABORT)" "$T/src.out")
kill_peer crash_dl
sleep 0.5
check receiver_crash_abort test "$(grep -c "bị huỷ (ABORT)" "$T/src.out")" -gt "$before"

# ---- 9. Tracker không giữ file: bộ nhớ đỉnh nhỏ dù đã relay hàng chục MB ----
hwm=$(awk '/VmHWM/ {print $2}' /proc/$TRACKER_PID/status)
echo "      tracker VmHWM = ${hwm} kB"
if grep -q libasan "/proc/$TRACKER_PID/maps"; then
    echo "SKIP  tracker_memory (build ASan làm RSS lớn hơn thực tế)"
else
    check tracker_memory test "$hwm" -lt 30000
fi

# ---- 10. (SLOW) ACK quá 10 s -> nguồn tự gửi lại ----
# 8 chunk đầu lấp đầy cửa sổ; ACK bị trễ 10.5 s nên nguồn hết hạn chờ và gửi lại
if [ -n "$SLOW" ]; then
    printf 'get src win.bin\nwait\nquit\n' |
        $BIN/peer 127.0.0.1 "$PORT" lateack --dl "$T/dl_lateack" --fault-delay=10500 \
        >"$T/lateack.out" 2>&1
    check ack_timeout_sha same_file "$F/win.bin" "$T/dl_lateack/win.bin"
    check ack_timeout_resend grep -q "chưa có ACK" "$T/src.out"
fi

# ---- 11. (SLOW) Nguồn im lặng 30 s -> Tracker huỷ phiên NO_PROGRESS ----
if [ -n "$SLOW" ]; then
    printf 'JOIN mute\nEXPECT OK\nSHARE x.bin 100000\nEXPECT OK\nTIMEOUT 5000\nEXPECT SEND_REQ\nTIMEOUT 40000\nEXPECT ABORT\n' |
        $BIN/fake_peer 127.0.0.1 "$PORT" mute >"$T/mute.out" 2>&1 &
    M=$!
    sleep 0.3
    printf 'get mute x.bin\nwait\nquit\n' |
        $BIN/peer 127.0.0.1 "$PORT" waiter --dl "$T/dl_waiter" >"$T/waiter.out" 2>&1
    check no_progress_receiver grep -q "NO_PROGRESS" "$T/waiter.out"
    wait $M
    check no_progress_source_abort test $? -eq 0
fi

stop_peer src
check tracker_alive kill -0 "$TRACKER_PID"
summary e2e_transfer
