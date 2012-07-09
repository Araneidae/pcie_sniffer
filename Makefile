kernelver := $(shell uname -r)
KBUILD_DIR = $(CURDIR)/kbuild-$(kernelver)
kernelsrc = /lib/modules/$(kernelver)/build

default: $(KBUILD_DIR)/fa_sniffer.ko

# Device version.  Advance on each release.
VERSION = 1.6-pre


KFILES = Kbuild fa_sniffer.c fa_sniffer.h

$(KBUILD_DIR):
	mkdir -p $(KBUILD_DIR)
	$(foreach file,$(KFILES), ln -s ../$(file) $(KBUILD_DIR);)

$(KBUILD_DIR)/fa_sniffer.ko: fa_sniffer.c fa_sniffer.h $(KBUILD_DIR)
	make -C $(kernelsrc) M=$(KBUILD_DIR) VERSION=$(VERSION) modules

NOTES.html: NOTES
	asciidoc $^

clean:
	rm -rf $(KBUILD_DIR) kbuild-* NOTES.html rpmbuild

.PHONY: clean default


SIZE=4096
COUNT=1
FILE=myfile.bin

insmod: $(KBUILD_DIR)/fa_sniffer.ko
	sudo /sbin/insmod $^

rmmod:
	sudo /sbin/rmmod fa_sniffer.ko

rpmbuild:
	mkdir rpmbuild

rpmbuild/%: % rpmbuild
	sed 's/@VERSION@/$(VERSION)/' $< >$@

rpm: rpmbuild/fa_sniffer.spec rpmbuild/dkms.conf
	mkdir -p rpmbuild/RPMS rpmbuild/BUILD
	rpmbuild -bb \
	    --define "_topdir $(CURDIR)/rpmbuild" \
            --define "_sourcedir $(CURDIR)" \
            --define '_tmppath %{_topdir}/BUILD' \
            rpmbuild/fa_sniffer.spec

.PHONY: insmod rmmod rpm
