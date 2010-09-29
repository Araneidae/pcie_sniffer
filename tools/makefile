# This makefile simply reflects the build to the $(BUILD_DIR) directory so that
# we can make out of directory.  The slightly naughty trick here is that make
# will first look for makefile (rather than Makefile) and we can then redirect
# to the $(BUILD_DIR) directory.

BUILD_DIR = build

all $(filter-out all $(BUILD_DIR) clean,$(MAKECMDGOALS)): $(BUILD_DIR)
	VPATH=$(CURDIR) $(MAKE) -C $(BUILD_DIR) -f ../Makefile $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: clean
