#!/bin/sh
# guest_run.sh - 在 QEMU guest 里以 root 运行，触发并观察页池的 double free。
#
# 前置：
#   1. host 上已经 make install（page_pool.ko 被复制到 kmodules/.../buddy_bug_lab/）
#   2. guest 里 9p 共享已经挂到 /mnt（./run_rlk_riscv.sh run 的 kmod_mount）
#   3. 以 root 运行（su root）
set -e

[ "$(id -u)" -eq 0 ] || { echo "请用 root 运行" >&2; exit 1; }

DIR=$(cd "$(dirname "$0")" && pwd)
MOD="$DIR/page_pool.ko"
[ -f "$MOD" ] || {
	echo "找不到 $MOD" >&2
	echo "先在 host 上进入该目录执行 make install" >&2
	exit 1
}

echo "== 基线 =="
grep -E 'MemFree|MemAvailable' /proc/meminfo
echo "== buddyinfo (order 0..10) =="
cat /proc/buddyinfo

echo
echo "== insmod：分配 8192 个 4KB 页（32 MiB），block 37 标记 cached =="
dmesg -C
insmod "$MOD"
dmesg

echo
echo "== rmmod：trim cached -> release 全池，观察 dmesg =="
rmmod page_pool
sleep 1
dmesg | tail -30

echo
echo "== 结束 =="
grep -E 'MemFree|MemAvailable' /proc/meminfo
cat /proc/buddyinfo

cat <<'TIP'

提示：
  * 上面的 pfn 每次启动都会变，用 dmesg 里 "trim cached block 37 pfn=..." 对照。
  * 如果这次没有 Bad page state，可能是 bad_page() 的 60 条/分钟抑制窗口，
    等一分钟或重启 guest 再试。
  * 想自己动手查，先别改代码：读 README.md 的“问题链”。
TIP
