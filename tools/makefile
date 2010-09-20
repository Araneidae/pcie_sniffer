# This makefile simply reflects the build to the $(BUILD) directory so that we
# can make out of directory.  The slightly naughty trick here is that make
# will first look for makefile (rather than Makefile) and we can then
# redirect to the $(BUILD) directory.

BUILD = build

all $(filter-out all $(BUILD) clean,$(MAKECMDGOALS)): $(BUILD)
	VPATH=$(CURDIR) $(MAKE) -C $(BUILD) -f ../Makefile $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: clean
