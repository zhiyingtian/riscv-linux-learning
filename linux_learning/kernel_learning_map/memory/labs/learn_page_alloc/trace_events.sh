#!/bin/bash
# trace_events.sh —— 抓取 mm_page_alloc tracepoint,验证一次匿名缺页分配的真实参数
#
# 用法:
#   sudo ./trace_events.sh [页数]          只抓触发进程(默认 64 页)
#   sudo ./trace_events.sh --all [秒数]    抓全系统(观察高阶/HighAtomic 分布)
# 需要:root(tracefs)+ gcc
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
TRACE=/sys/kernel/tracing
[ -d "$TRACE/events" ] || TRACE=/sys/kernel/debug/tracing

if [ ! -d "$TRACE/events/kmem" ]; then
    echo "[!] tracefs 未就绪:$TRACE/events/kmem 不存在" >&2
    exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "[!] 需要 root:sudo $0 $1" >&2
    exit 1
fi

if [ ! -x "$DIR/trigger_alloc" ] || [ "$DIR/trigger_alloc.c" -nt "$DIR/trigger_alloc" ]; then
    gcc -O0 -g -o "$DIR/trigger_alloc" "$DIR/trigger_alloc.c"
fi

cleanup() {
    echo 0 > "$TRACE/tracing_on" 2>/dev/null || true
    echo 0 > "$TRACE/events/kmem/mm_page_alloc/enable" 2>/dev/null || true
    : > "$TRACE/set_event_pid" 2>/dev/null || true
}
trap cleanup EXIT

echo "== 1. 复位 tracer =="
echo "宿主内核:$(uname -r)  (学习目标 5.15;tracepoint 字段跨版本一致,参数结论通用)"
cleanup
: > "$TRACE/trace"
echo 1 > "$TRACE/events/kmem/mm_page_alloc/enable"

echo "== 2. 开始跟踪 =="
if [ "$MODE" = pid ]; then
    OUT=$(mktemp)
    "$DIR/trigger_alloc" "$N" > "$OUT" 2>&1 &
    TPID=$!
    echo "$TPID" > "$TRACE/set_event_pid"
    echo "只抓触发进程 pid=$TPID"
    echo 1 > "$TRACE/tracing_on"
    wait "$TPID"
    cat "$OUT"
    rm -f "$OUT"
else
    echo "全系统模式,采集 ${SECS}s ..."
    echo 1 > "$TRACE/tracing_on"
    sleep "$SECS"
fi
sleep 0.2
echo 0 > "$TRACE/tracing_on"
: > "$TRACE/set_event_pid"

RAW=$(grep "mm_page_alloc:" "$TRACE/trace" || true)
CNT=$(printf '%s\n' "$RAW" | grep -c "mm_page_alloc:" || true)
[ -n "$RAW" ] || { echo "[!] 没抓到任何事件" >&2; exit 1; }

echo
echo "== 3. 结果 =="
printf 'mm_page_alloc 事件数:%s\n\n' "$CNT"

echo "--- 前 8 条原始事件 ---"
printf '%s\n' "$RAW" | sed -n '1,8p'

echo
echo "--- order 分布 ---"
printf '%s\n' "$RAW" | grep -oE "order=[0-9]+" | sort | uniq -c | sort -rn

echo "--- migratetype 分布(0=Unmovable 1=Movable 2=Reclaimable 3=HighAtomic 4=CMA) ---"
printf '%s\n' "$RAW" | grep -oE "migratetype=[0-9]+" | sort | uniq -c | sort -rn

echo "--- gfp_flags 分布(前 6)---"
printf '%s\n' "$RAW" | grep -oE "gfp_flags=[^ ]+" | sort | uniq -c | sort -rn | sed -n '1,6p'

echo
echo "== 4. 对照框架 =="
echo "  alloc_zeroed_user_highpage_movable → order 应为 0"
echo "  GFP_HIGHUSER_MOVABLE               → migratetype 应为 1(Movable)+ __GFP_ZERO"
echo "  若 migratetype=0 或 gfp 无 MOVABLE → 说明触发的不是匿名缺页路径"
