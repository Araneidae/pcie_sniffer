KFILES = Kbuild fa_sniffer.c
KBUILD = kbuild-$(shell uname -r)

all: $(KBUILD)/fa_sniffer.ko tools NOTES.html

$(KBUILD):
	mkdir -p $(KBUILD)
	$(foreach file,$(KFILES), ln -s ../$(file) $(KBUILD);)

$(KBUILD)/fa_sniffer.ko: fa_sniffer.c $(KBUILD)
	make -C /lib/modules/$(shell uname -r)/build \
            M=$(CURDIR)/$(KBUILD) modules

tools:
	make -C tools

NOTES.html: NOTES
	asciidoc $^

clean:
	rm -rf kbuild-* NOTES.html
	make -C tools clean

.PHONY: tools clean all


SIZE=4096
COUNT=1
FILE=myfile.bin

test: $(KBUILD)/fa_sniffer.ko
	./runtest bs=$(SIZE) count=$(COUNT) >$(FILE)

insmod: $(KBUILD)/fa_sniffer.ko
	sudo /sbin/insmod $^

rmmod:
	sudo /sbin/rmmod fa_sniffer.ko

.PHONY: test insmod rmmod
