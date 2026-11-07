obj-m := page_refcount.o

KDIR ?= /home/alan/workspace/riscv/runninglinuxkernel_5.15
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-
SHARED ?= $(KDIR)/kmodules/learn_page_alloc/page_refcount_bug_lab

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

install: all
	mkdir -p $(SHARED)
	cp page_refcount.ko guest_run.sh README.md $(SHARED)/
	chmod 644 $(SHARED)/page_refcount.ko $(SHARED)/README.md
	chmod 755 $(SHARED)/guest_run.sh
	@echo "installed: $(SHARED)/page_refcount.ko"
	@echo "           $(SHARED)/guest_run.sh"
	@echo "           $(SHARED)/README.md"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -f page_refcount.ko

.PHONY: all install clean
