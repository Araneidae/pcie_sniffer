KFILES = Kbuild fa_sniffer.c

all: kbuild/fa_sniffer.ko

kbuild:
	mkdir -p kbuild
	$(foreach file,$(KFILES), ln -s ../$(file) kbuild;)

kbuild/fa_sniffer.ko: fa_sniffer.c kbuild
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/kbuild modules

clean:
	rm -rf kbuild


SIZE=4096
COUNT=1
FILE=myfile.bin

test: kbuild/fa_sniffer.ko
	sudo ./runtest bs=$(SIZE) count=$(COUNT) >$(FILE)
