KFILES = Kbuild fa_sniffer.c

all: kbuild/fa_sniffer.ko tools NOTES.html

kbuild:
	mkdir -p kbuild
	$(foreach file,$(KFILES), ln -s ../$(file) kbuild;)

kbuild/fa_sniffer.ko: fa_sniffer.c kbuild
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/kbuild modules

tools:
	make -C tools

NOTES.html: NOTES
	asciidoc $^

clean:
	rm -rf kbuild NOTES.html
	make -C tools clean 

.PHONY: tools clean all


SIZE=4096
COUNT=1
FILE=myfile.bin

test: kbuild/fa_sniffer.ko
	./runtest bs=$(SIZE) count=$(COUNT) >$(FILE)

insmod: kbuild/fa_sniffer.ko
	sudo /sbin/insmod $^

rmmod:
	sudo /sbin/rmmod fa_sniffer.ko

.PHONY: test insmod rmmod
