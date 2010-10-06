kernelver := $(shell uname -r)
KBUILD_DIR = $(CURDIR)/kbuild-$(kernelver)
kernelsrc = /lib/modules/$(kernelver)/build

default: $(KBUILD_DIR)/fa_sniffer.ko


KFILES = Kbuild fa_sniffer.c

$(KBUILD_DIR):
	mkdir -p $(KBUILD_DIR)
	$(foreach file,$(KFILES), ln -s ../$(file) $(KBUILD_DIR);)

$(KBUILD_DIR)/fa_sniffer.ko: fa_sniffer.c $(KBUILD_DIR)
	make -C $(kernelsrc) M=$(KBUILD_DIR) modules

NOTES.html: NOTES
	asciidoc $^

clean:
	rm -rf $(KBUILD_DIR) kbuild-* NOTES.html

.PHONY: clean default


SIZE=4096
COUNT=1
FILE=myfile.bin

test: $(KBUILD_DIR)/fa_sniffer.ko
	./runtest bs=$(SIZE) count=$(COUNT) >$(FILE)

insmod: $(KBUILD_DIR)/fa_sniffer.ko
	sudo /sbin/insmod $^

rmmod:
	sudo /sbin/rmmod fa_sniffer.ko

.PHONY: test insmod rmmod
