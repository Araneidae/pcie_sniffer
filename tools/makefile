# This makefile simply reflects the build to the build directory so that we
# can make out of directory.  The slightly naughty trick here is that make
# will first look for makefile (rather than Makefile) and we can then
# redirect to the build directory.

all $(filter-out all build clean,$(MAKECMDGOALS)): build
	VPATH=$(CURDIR) $(MAKE) -C build -f ../Makefile $@

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: clean
