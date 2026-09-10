obj-m += l2switch.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod l2switch.ko+++

unload:
	sudo rmmod l2switch

reload: unload load

logs:
	dmesg | tail -50