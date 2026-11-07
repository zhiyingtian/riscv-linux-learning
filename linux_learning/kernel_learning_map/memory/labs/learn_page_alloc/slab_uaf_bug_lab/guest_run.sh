#!/bin/sh
# guest_run.sh - slab UAF / double-free / OOB 演示。
# 默认只跑相对安全的 mode=1 和正确的 mode=4。
# mode=2/3 可能破坏 guest，请先保存快照再手动运行。
set -e

[ "$(id -u)" -eq 0 ] || { echo "请用 root 运行" >&2; exit 1; }

DIR=$(cd "$(dirname "$0")" && pwd)
MOD="$DIR/slab_uaf.ko"
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
	rmmod slab_uaf
	dmesg | tail -40
}

run_mode 1 "UAF 别名：kfree 后旧指针读取，观察是否复用同一地址"
run_mode 4 "正确：kmalloc + use + kfree"

cat <<'TIP'

危险模式（手动、先做快照）：
  insmod slab_uaf.ko mode=2   # kfree 两次，可能破坏 freelist
  insmod slab_uaf.ko mode=3   # kmalloc(32) 写 64 字节，可能破坏相邻对象

提示：
  * mode=1 的 stale 指针在 kfree 后就是悬空指针；demo 读它只是为了
    展示 UAF，真实代码不应再访问。
  * 当前内核 CONFIG_SLUB_DEBUG=y 但 slub_debug 默认没开，double free
    不一定立刻报错；可以在内核 cmdline 加 slub_debug=FZPU 再启动。
  * 当前内核没有 KASAN/KFENCE；想稳定抓 UAF/越界，建议单独编译
    debug 内核（KASAN 或 KFENCE + SLUB_DEBUG_ON）。
TIP
