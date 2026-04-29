ifneq ($(KERNELRELEASE),)

obj-m += rpi5_ili9341.o
rpi5_ili9341-y := src/rpi5_ili9341.o
ccflags-y += -I$(src)/include

else

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
DTC ?= dtc
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

.PHONY: all module tools dtbo clean help

all: module tools dtbo

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

tools: tools/ili9341_demo

tools/ili9341_demo: tools/ili9341_demo.c include/uapi/linux/rpi5_ili9341.h
	$(CC) $(CFLAGS) -I$(PWD)/include/uapi -o $@ $<

dtbo: overlay/rpi5-ili9341-spi0.dtbo

overlay/rpi5-ili9341-spi0.dtbo: overlay/rpi5-ili9341-spi0.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean
	-$(RM) tools/ili9341_demo overlay/*.dtbo

help:
	@echo "make module   - Build the kernel module"
	@echo "make tools    - Build the user-space demo utility"
	@echo "make dtbo     - Build the Raspberry Pi device-tree overlay"
	@echo "make all      - Build everything"
	@echo "make clean    - Remove build artefacts"

endif
