obj-m = memguard.o
KERNEL_SRC := /home/c674s876/Documents/firesim/DOSvBRU-firesim/target-design/chipyard/software/firemarshal/boards/firechip/linux-fsim/
CROSS_COMPILE := riscv64-unknown-linux-gnu-
ARCH := riscv
PWD := $(CURDIR)
EXTRA_CFLAGS += -DFIRESIM
all: 
	make -C $(KERNEL_SRC) M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE)  ARCH=$(ARCH) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" modules


plic:
	riscv64-unknown-linux-gnu-gcc plic_en.c -o plic_en
	chmod +x plic_en


clean:
	make -C $(BLDDIR) M=$(PWD) clean
