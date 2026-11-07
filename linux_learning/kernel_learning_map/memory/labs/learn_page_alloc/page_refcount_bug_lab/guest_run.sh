#!/bin/sh
# guest_run.sh - 对比物理页引用计数泄漏和多余 put_page。
# 在 guest 里以 root 运行。
set -e

[ "$(id -u)" -eq 0 ] || { echo "请用 root 运行" >&2; exit 1; }

DIR=$(cd "$(dirname "$0")" && pwd)
MOD="$DIR/page_refcount.ko"
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
	rmmod page_refcount
	dmesg | tail -40
}

run_mode 1 "多一次 get_page，少一次 put_page：页泄漏"
run_mode 2 "多一次 put_page：页提前释放，后续访问会 UAF"
run_mode 3 "正确：一次 alloc_pages + 一次 __free_pages"

echo
echo "== taint =="
cat /proc/sys/kernel/tainted

cat <<'TIP'

提示：
  * refcount=0 表示页已经回到 allocator；此时继续用 struct page 或页内容
    就是 use-after-free。
  * mode=1 打印的 refcount=1 说明页没有真正释放；demo 为了不污染 guest，
    在退出前补了一次 put_page。
  * 当前内核 CONFIG_DEBUG_VM=n，很多 refcount 断言不会触发；不要因为
    没看到 BUG 就认为代码正确。
TIP
