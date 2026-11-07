#!/bin/bash
# trace_graph.sh —— 抓页分配器的内部调用链,和框架对照
# 用法:sudo ./trace_graph.sh [页数]        默认 8 页(函数图很啰嗦,页数别太大)
# 需要:root(tracefs)+ gcc
#
# 【版本提醒】抓的是【宿主内核】的路径(当前 7.0),而学习目标是 5.15。
#   7.0 重构过 page allocator:__alloc_pages → __alloc_pages_noprof,
#   且 rmqueue_pcplist / __rmqueue_pcplist / prep_new_page / prepare_alloc_pages
#   已被内联或改名,function_graph 追不到。要精确对齐 5.15,需在 qemu 里跑 5.15。
set -e

N=${1:-8}
DIR=$(cd "$(dirname "$0")" && pwd)
TRACE=/sys/kernel/tracing
[ -d "$TRACE/events" ] || TRACE=/sys/kernel/debug/tracing

if [ "$(id -u)" -ne 0 ]; then
    echo "[!] 需要 root:sudo $0 $N" >&2
    exit 1
fi

if [ ! -x "$DIR/trigger_alloc" ] || [ "$DIR/trigger_alloc.c" -nt "$DIR/trigger_alloc" ]; then
    gcc -O0 -g -o "$DIR/trigger_alloc" "$DIR/trigger_alloc.c"
fi

# ---- 探测可跟踪符号(5.15 用 __alloc_pages;6.8+ 用 __alloc_pages_noprof)----
SYM=""
for cand in __alloc_pages_noprof __alloc_pages; do
    if grep -qx "$cand" "$TRACE/available_filter_functions" 2>/dev/null; then
        SYM=$cand
        break
    fi
done
if [ -z "$SYM" ]; then
    echo "[!] 找不到可跟踪的 __alloc_pages 符号(可用:grep alloc_pages $TRACE/available_filter_functions)" >&2
    exit 1
fi

echo "宿主内核 : $(uname -r)   (学习目标: 5.15.0)"
echo "跟踪符号 : $SYM"

cleanup() {
    echo 0 > "$TRACE/tracing_on" 2>/dev/null || true
    echo nop > "$TRACE/current_tracer" 2>/dev/null || true
    : > "$TRACE/set_graph_function" 2>/dev/null || true
    : > "$TRACE/set_ftrace_filter" 2>/dev/null || true
}
trap cleanup EXIT

echo "== 1. 配置 function_graph,只跟 $SYM =="
cleanup
: > "$TRACE/trace"
echo function_graph > "$TRACE/current_tracer"
if ! echo "$SYM" > "$TRACE/set_graph_function" 2>/dev/null; then
    echo "[i] set_graph_function 写入失败,回退到 set_ftrace_filter"
    echo "$SYM" > "$TRACE/set_ftrace_filter"
fi
echo 1 > "$TRACE/tracing_on"

echo "== 2. 触发 $N 页缺页 =="
"$DIR/trigger_alloc" "$N"
sleep 1

echo 0 > "$TRACE/tracing_on"

echo
echo "== 3. 调用链(取一次分配)=="
grep -A40 -F "$SYM()" "$TRACE/trace" | head -60 || true

echo
echo "== 4. 关键函数出现次数 =="
echo "   (标注「本内核不存在」的是被内联/改名,不是「没调用」)"
for f in prepare_alloc_pages get_page_from_freelist rmqueue rmqueue_pcplist \
         __rmqueue_pcplist rmqueue_bulk prep_new_page post_alloc_hook \
         __rmqueue __rmqueue_smallest __alloc_pages_slowpath node_reclaim; do
    if grep -qx "$f" "$TRACE/available_filter_functions" 2>/dev/null; then
        c=$(grep -c -F "$f()" "$TRACE/trace" 2>/dev/null || true)
        printf "  %-24s %s\n" "$f" "${c:-0}"
    else
        printf "  %-24s (本内核不存在/已内联)\n" "$f"
    fi
done

echo
echo "== 5. 怎么读这个结果 =="
echo "  · 宿主 7.0 只能验证【骨架】:__alloc_pages → get_page_from_freelist → rmqueue_bulk"
echo "  · 你框架里的 rmqueue_pcplist / __rmqueue_pcplist 分层,7.0 已重构,看不到 ≠ 你写错"
echo "  · 想逐行对照 5.15 的 rmqueue_pcplist / prep_new_page,请在 qemu 的 5.15 里跑同一脚本"
echo "  · 参数类验证(order/migratetype/gfp_flags)不受影响,继续用 trace_events.sh"
