#!/bin/sh
# guest_run.sh - 在 QEMU guest 里以 root 运行，对比三种 GFP/上下文组合。
#
# 前置：
#   1. host 上已经 make install
#   2. guest 里 9p 共享已经挂到 /mnt
#   3. 以 root 运行（su root）
set -e

[ "$(id -u)" -eq 0 ] || { echo "请用 root 运行" >&2; exit 1; }

DIR=$(cd "$(dirname "$0")" && pwd)
MOD="$DIR/gfp_context.ko"
[ -f "$MOD" ] || {
	echo "找不到 $MOD" >&2
	echo "先在 host 上进入该目录执行 make install" >&2
	exit 1
}

run_mode() {
	mode=$1
	echo
	echo "============================================================"
	echo "== mode=$mode：$2"
	echo "============================================================"
	dmesg -C
	insmod "$MOD" mode="$mode"
	rmmod gfp_context
	dmesg | tail -100
}

run_mode 1 "原子上下文 + GFP_KERNEL（错误，预期 sleeping function 警告）"
run_mode 2 "原子上下文 + GFP_ATOMIC（正确，预期无警告）"
run_mode 3 "进程上下文 + GFP_KERNEL（对照，预期无警告）"

echo
echo "== taint =="
cat /proc/sys/kernel/tainted

cat <<'TIP'

提示：
  * mode=1 的 "BUG: sleeping function called from invalid context" 是
    might_sleep() 打出来的；它不代表这次一定真的睡了，而是说“当前上下文
    不允许睡眠，而你用了可能睡眠的 GFP_KERNEL”。
  * 如果 mode=1 没有警告，先确认 CONFIG_DEBUG_ATOMIC_SLEEP=y；
    might_sleep() 还有 1 秒的限流，连续快速重跑可能看不到。
  * taint 里的 W 是 TAINT_WARN（512），dmesg -C 不会清掉它，重启才清。
TIP
