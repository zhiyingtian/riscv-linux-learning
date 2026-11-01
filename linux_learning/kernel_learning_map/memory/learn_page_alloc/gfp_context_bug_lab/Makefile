obj-m := gfp_context.o

KDIR ?= /home/alan/workspace/riscv/runninglinuxkernel_5.15
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-
SHARED ?= $(KDIR)/kmodules/learn_page_alloc/gfp_context_bug_lab

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

install: all
	mkdir -p $(SHARED)
	cp gfp_context.ko guest_run.sh README.md gfp_keypoints.md $(SHARED)/
	chmod 644 $(SHARED)/gfp_context.ko $(SHARED)/README.md $(SHARED)/gfp_keypoints.md
	chmod 755 $(SHARED)/guest_run.sh
	@echo "installed: $(SHARED)/gfp_context.ko"
	@echo "           $(SHARED)/guest_run.sh"
	@echo "           $(SHARED)/README.md"
	@echo "           $(SHARED)/gfp_keypoints.md"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -f gfp_context.ko

.PHONY: all install clean
