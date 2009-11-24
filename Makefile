# obj-m += hello-1.o
obj-m += fa_sniffer.o

ifneq ($(shell hostname -s),pc0046)
$(error This make file needs to be run on pc46)
endif

EXTRA_CFLAGS += -O2 -Werror -Wextra
EXTRA_CFLAGS += -Wno-declaration-after-statement -Wno-unused-parameter
EXTRA_CFLAGS += -Wno-missing-field-initializers
export EXTRA_CFLAGS

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f Module.markers Module.symvers

test: # all
	sudo /sbin/insmod ./fa_sniffer.ko
	sudo dd if=/dev/fa_sniffer bs=4096 count=1 | hexdump -C
	sudo /sbin/rmmod ./fa_sniffer.ko
