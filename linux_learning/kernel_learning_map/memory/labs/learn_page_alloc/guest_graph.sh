#!/bin/bash
# guest_graph.sh —— 在 qemu(5.15)guest 里抓 __alloc_pages 的完整调用链
# 用法:./guest_graph.sh [页数]        默认 8 页
# 前提:内核已开 CONFIG_FUNCTION_TRACER + CONFIG_FUNCTION_GRAPH_TRACER(已重编)
set -e

N=${1:-8}
DIR=$(cd "$(dirname "$0")" && pwd)

[ "$(id -u)" -eq 0 ] || { echo "[!] 需要 root" >&2; exit 1; }

# ---- tracefs:自动探测 / 必要时挂载(同 guest_run.sh)----
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
[ -d "$TRACE/events" ] || { echo "[!] tracefs 不可用" >&2; exit 1; }
echo "tracefs: $TRACE"

# 5.15 没有 _noprof 后缀,真实符号就是 __alloc_pages
BIN=$DIR/trigger_alloc_riscv64
[ -x "$BIN" ] || { echo "[!] 找不到 $BIN" >&2; exit 1; }

SYM=__alloc_pages
if ! grep -qx "$SYM" "$TRACE/available_filter_functions" 2>/dev/null; then
    echo "[!] $SYM 不可跟踪,检查 CONFIG_FUNCTION_TRACER" >&2
    exit 1
fi

cleanup() {
    echo 0 > "$TRACE/tracing_on" 2>/dev/null || true
    echo nop > "$TRACE/current_tracer" 2>/dev/null || true
    : > "$TRACE/set_graph_function" 2>/dev/null || true
    : > "$TRACE/set_ftrace_pid" 2>/dev/null || true
}
trap cleanup EXIT

echo "== 内核:$(uname -r) =="
cleanup
: > "$TRACE/trace"
echo function_graph > "$TRACE/current_tracer"
echo "$SYM" > "$TRACE/set_graph_function"

OUT=/tmp/trigger_alloc.out
"$BIN" "$N" > "$OUT" 2>&1 &
TPID=$!
echo "$TPID" > "$TRACE/set_ftrace_pid"
echo "== 只跟触发进程 pid=$TPID =="
echo 1 > "$TRACE/tracing_on"
wait "$TPID"
sleep 0.2
echo 0 > "$TRACE/tracing_on"
: > "$TRACE/set_ftrace_pid"
cat "$OUT"

echo
echo "== 调用链(取一次分配)=="
grep -A45 -F "$SYM()" "$TRACE/trace" | sed -n '1,60p'

echo
echo "== 关键函数出现次数 =="
for f in prepare_alloc_pages get_page_from_freelist zone_watermark_fast rmqueue \
         rmqueue_pcplist __rmqueue_pcplist rmqueue_bulk prep_new_page \
         __rmqueue __rmqueue_smallest __alloc_pages_slowpath node_reclaim; do
    c=$(grep -c -F "$f()" "$TRACE/trace" 2>/dev/null || true)
    printf "  %-24s %s\n" "$f" "${c:-0}"
done

echo
echo "== 对照 phy_mem_allocate.c 的三个问题 =="
echo "  ① rmqueue_bulk 是否出现?→ 出现 = pcp 空时【内部从 buddy 批量补充】,"
echo "     不是「返回 NULL 后再从 freelist 分配」"
echo "  ② order=0 时 __rmqueue / __rmqueue_smallest 应为 0 次(order<=3 一律走 pcp)"
echo "  ③ __alloc_pages_slowpath 正常应为 0 次"
