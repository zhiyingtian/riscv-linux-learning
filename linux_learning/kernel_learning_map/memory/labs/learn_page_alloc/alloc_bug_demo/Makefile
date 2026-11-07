obj-m := alloc_bug_demo.o

# 指向 5.15 源码树(编译模块必须与目标内核的 .config/Module.symvers 一致)
KDIR ?= /home/alan/workspace/riscv/runninglinuxkernel_5.15
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-

# qemu 9p 共享目录(guest 里挂载为 kmod_mount)
SHARED ?= /home/alan/workspace/riscv/runninglinuxkernel_5.15/kmodules/learn_page_alloc/alloc_bug_demo

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

# 编译并同步到 guest 可见的共享目录
install: all
	mkdir -p $(SHARED)
	cp alloc_bug_demo.ko $(SHARED)/
	@echo "已复制到 $(SHARED)/alloc_bug_demo.ko"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

# 自检:能编译 + 产物存在 + vermagic 与目标内核一致,最后清理中间文件
test: all
	@test -f alloc_bug_demo.ko || { echo "FAIL: alloc_bug_demo.ko 未生成"; exit 1; }
	@krel=$$($(MAKE) -s -C $(KDIR) kernelrelease); \
	 if strings alloc_bug_demo.ko | grep -q "^vermagic=$$krel "; then \
		echo "PASS: vermagic=$$krel"; \
	 else \
		echo "FAIL: vermagic 与内核 $$krel 不匹配"; exit 1; \
	 fi
	@$(MAKE) -s clean
	@rm -f Module.symvers modules.order

.PHONY: all install test clean
