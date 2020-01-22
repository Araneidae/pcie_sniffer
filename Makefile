# Device version.  Advance on each release.
VERSION = 1.10
RELEASE = 1dkms


kernelver := $(shell uname -r)
KBUILD_DIR = $(CURDIR)/kbuild-$(kernelver)
kernelsrc = /lib/modules/$(kernelver)/build

default: $(KBUILD_DIR)/fa_sniffer.ko

RPM_FILE = fa_sniffer-$(VERSION)-$(RELEASE).noarch.rpm

# Files used in build of sniffer device
KFILES = Kbuild fa_sniffer.c fa_sniffer.h

# The RPM depends on the following files, also listed in fa_sniffer.spec
RPM_DEPENDS = $(KFILES) Makefile 11-fa_sniffer.rules


$(KBUILD_DIR):
	mkdir -p $(KBUILD_DIR)
	$(foreach file,$(KFILES), ln -s ../$(file) $(KBUILD_DIR);)

$(KBUILD_DIR)/fa_sniffer.ko: $(KFILES) $(KBUILD_DIR)
	make -C $(kernelsrc) M=$(KBUILD_DIR) VERSION=$(VERSION) modules

NOTES.html: NOTES
	rst2html $^ >$@

clean:
	rm -rf $(KBUILD_DIR) kbuild-* NOTES.html rpmbuild *.rpm

.PHONY: clean default


# Used for dd test target
DD_SIZE = 2048
DD_COUNT = 1

insmod: $(KBUILD_DIR)/fa_sniffer.ko
	sudo sh -c 'echo 7 >/proc/sys/kernel/printk'
	lsmod | grep -q fa_sniffer && sudo /sbin/rmmod fa_sniffer; true
	sudo /sbin/insmod $^

rmmod:
	sudo /sbin/rmmod fa_sniffer.ko

dd:
	dd if=/dev/fa_sniffer0 bs=$(DD_SIZE) count=$(DD_COUNT) | hexdump -C

rpmbuild:
	mkdir rpmbuild

rpmbuild/%: % rpmbuild Makefile
	sed 's/@VERSION@/$(VERSION)/;s/@RELEASE@/$(RELEASE)/' $< >$@

rpm: rpmbuild/fa_sniffer.spec rpmbuild/dkms.conf $(RPM_DEPENDS)
	mkdir -p rpmbuild/RPMS rpmbuild/BUILD
	rpmbuild -bb \
	    --define '_topdir $(CURDIR)/rpmbuild' \
            --define '_sourcedir $(CURDIR)' \
            --define '_tmppath %{_topdir}/BUILD' \
            rpmbuild/fa_sniffer.spec
	ln -sf rpmbuild/RPMS/noarch/$(RPM_FILE) .

.PHONY: insmod rmmod rpm
