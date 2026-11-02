obj-m := slab_uaf.o

KDIR ?= /home/alan/workspace/riscv/runninglinuxkernel_5.15
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-
SHARED ?= $(KDIR)/kmodules/learn_page_alloc/slab_uaf_bug_lab

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

install: all
	mkdir -p $(SHARED)
	cp slab_uaf.ko guest_run.sh README.md $(SHARED)/
	chmod 644 $(SHARED)/slab_uaf.ko $(SHARED)/README.md
	chmod 755 $(SHARED)/guest_run.sh
	@echo "installed: $(SHARED)/slab_uaf.ko"
	@echo "           $(SHARED)/guest_run.sh"
	@echo "           $(SHARED)/README.md"

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
	rm -f slab_uaf.ko

.PHONY: all install clean
