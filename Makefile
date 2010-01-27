SIZE=4096
COUNT=1
FILE=myfile.bin

# obj-m += hello-1.o
obj-m += fa_sniffer.o

ifneq ($(shell hostname -s),pc0046)
$(error This make file needs to be run on pc46)
endif

# EXTRA_CFLAGS += -std=gnu99
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
	sudo ./runtest bs=$(SIZE) count=$(COUNT) >$(FILE)
