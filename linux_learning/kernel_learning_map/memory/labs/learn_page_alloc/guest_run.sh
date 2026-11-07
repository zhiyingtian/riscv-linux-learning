#!/bin/bash
# guest_run.sh —— 在 qemu(5.15)guest 里跑 Q3 实验:匿名缺页的物理页从哪来
#
# 用法(guest 内,root):
#   ./guest_run.sh [页数]        只抓触发进程(默认 64)
#   ./guest_run.sh --all [秒数]  抓全系统(看高阶分配/HighAtomic)
#
# 前提:内核 CONFIG_FTRACE=y(已确认),tracepoint 可用;
#       CONFIG_FUNCTION_TRACER 未开,所以本脚本只做 tracepoint,不做 function_graph。
set -e

MODE=pid
if [ "$1" = "--all" ]; then
    MODE=all
    SECS=${2:-5}
    N=1
else
    N=${1:-64}
fi

DIR=$(cd "$(dirname "$0")" && pwd)

[ "$(id -u)" -eq 0 ] || { echo "[!] 需要 root" >&2; exit 1; }

# ---- tracefs:自动探测 / 必要时挂载 ----
# guest 的 /etc/fstab 里没有 tracefs 项,只能靠 systemd 的 sys-kernel-tracing.mount,
# 若 systemd 没起来(init=/bin/sh、rescue 等)就必须自己挂。
TRACE=""
for t in /sys/kernel/tracing /sys/kernel/debug/tracing; do
    if [ -d "$t/events" ]; then
        TRACE=$t
        break
    fi
done
if [ -z "$TRACE" ]; then
    TRACE=/sys/kernel/tracing
    mkdir -p "$TRACE"
    if ! mount -t tracefs tracefs "$TRACE" 2>/dev/null; then
        mkdir -p /sys/kernel/debug
        mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
        if [ -d /sys/kernel/debug/tracing/events ]; then
            TRACE=/sys/kernel/debug/tracing
        fi
    fi
fi
[ -d "$TRACE/events/kmem/mm_page_alloc" ] || {
    echo "[!] tracefs 或 mm_page_alloc 事件不可用(尝试过 /sys/kernel/tracing 和 /sys/kernel/debug/tracing)" >&2
    echo "    检查:CONFIG_TRACEFS / CONFIG_FTRACE / mount | grep trace" >&2
    exit 1
}
echo "tracefs: $TRACE"

# 优先用交叉编译好的静态二进制
BIN=$DIR/trigger_alloc_riscv64
if [ ! -x "$BIN" ]; then
    if [ -x "$DIR/trigger_alloc" ]; then
        BIN=$DIR/trigger_alloc
    elif command -v gcc >/dev/null 2>&1; then
        gcc -O0 -g -o "$DIR/trigger_alloc" "$DIR/trigger_alloc.c"
        BIN=$DIR/trigger_alloc
    else
        echo "[!] 找不到 trigger_alloc_riscv64,guest 里也没有 gcc" >&2
        exit 1
    fi
fi

cleanup() {
    echo 0 > "$TRACE/tracing_on" 2>/dev/null || true
    echo 0 > "$TRACE/events/kmem/mm_page_alloc/enable" 2>/dev/null || true
    : > "$TRACE/set_event_pid" 2>/dev/null || true
}
trap cleanup EXIT

echo "== 内核:$(uname -r) =="
cleanup
: > "$TRACE/trace"
echo 1 > "$TRACE/events/kmem/mm_page_alloc/enable"

if [ "$MODE" = pid ]; then
    OUT=/tmp/trigger_alloc.out
    "$BIN" "$N" > "$OUT" 2>&1 &
    TPID=$!
    echo "$TPID" > "$TRACE/set_event_pid"
    echo "== 只抓触发进程 pid=$TPID =="
    echo 1 > "$TRACE/tracing_on"
    wait "$TPID"
    cat "$OUT"
else
    echo "== 全系统采集 ${SECS}s =="
    echo 1 > "$TRACE/tracing_on"
    sleep "$SECS"
fi
sleep 0.2
echo 0 > "$TRACE/tracing_on"
: > "$TRACE/set_event_pid"

RAW=$(grep "mm_page_alloc:" "$TRACE/trace" || true)
[ -n "$RAW" ] || { echo "[!] 没抓到事件" >&2; exit 1; }

printf '\n== 事件数:%s ==\n\n' "$(printf '%s\n' "$RAW" | grep -c "mm_page_alloc:")"

echo "--- 前 4 条 ---"
printf '%s\n' "$RAW" | sed -n '1,4p'

echo
echo "--- order 分布 ---"
printf '%s\n' "$RAW" | grep -oE "order=[0-9]+" | sort | uniq -c | sort -rn

echo "--- migratetype 分布(0=Unmovable 1=Movable 2=Reclaimable 3=HighAtomic 4=CMA) ---"
printf '%s\n' "$RAW" | grep -oE "migratetype=[0-9]+" | sort | uniq -c | sort -rn

echo "--- gfp_flags 分布(前 5)---"
printf '%s\n' "$RAW" | grep -oE "gfp_flags=[^ ]+" | sort | uniq -c | sort -rn | sed -n '1,5p'

echo
echo "== 对照 phy_mem_allocate.c =="
echo "  alloc_zeroed_user_highpage_movable → order=0,migratetype=1(Movable),gfp 含 __GFP_ZERO"
echo "  5.15 的 GFP_HIGHUSER_MOVABLE 不带 __GFP_COMP(7.0 才多出来)"
