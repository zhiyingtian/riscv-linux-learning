obj-m := page_pool.o

KDIR ?= /home/alan/workspace/riscv/runninglinuxkernel_5.15
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-
SHARED ?= $(KDIR)/kmodules/learn_page_alloc/buddy_bug_lab

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

install: all
	mkdir -p $(SHARED)
	cp page_pool.ko guest_run.sh README.md double_free_keypoints.md $(SHARED)/
	chmod 644 $(SHARED)/page_pool.ko $(SHARED)/README.md $(SHARED)/double_free_keypoints.md
	chmod 755 $(SHARED)/guest_run.sh
	@echo "installed: $(SHARED)/page_pool.ko"
	@echo "           $(SHARED)/guest_run.sh"
	@echo "           $(SHARED)/README.md"
	@echo "           $(SHARED)/double_free_keypoints.md"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -f page_pool.ko

.PHONY: all install clean
