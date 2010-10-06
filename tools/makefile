# This makefile simply reflects the build to the $(BUILD_DIR) directory so that
# we can make out of directory.  The slightly naughty trick here is that make
# will first look for makefile (rather than Makefile) and we can then redirect
# to the $(BUILD_DIR) directory.

BUILD_DIR = build

all $(filter-out all clean $(BUILD_DIR),$(MAKECMDGOALS)): $(BUILD_DIR)
	VPATH=$(CURDIR) $(MAKE) \
            SRCDIR=$(CURDIR) -C $(BUILD_DIR) -f $(CURDIR)/Makefile $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
