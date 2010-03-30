all:    # Default target comes first!

CFLAGS += -std=gnu99
CFLAGS += -O2 -Werror -Wall -Wextra
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers

CPPFLAGS += -D_GNU_SOURCE
CPPFLAGS += -D_FILE_OFFSET_BITS=64

LDFLAGS += -lpthread -lrt


BUILD = archiver read prepare

archiver_SRCS += archiver.c
archiver_SRCS += error.c
archiver_SRCS += buffer.c
archiver_SRCS += sniffer.c
archiver_SRCS += disk_writer.c
archiver_SRCS += disk.c

read_SRCS += read.c
read_SRCS += error.c
read_SRCS += mask.c
read_SRCS += disk.c

prepare_SRCS += prepare.c
prepare_SRCS += error.c
prepare_SRCS += disk.c


all: $(BUILD)

define expand_build
$(target): $($(target)_SRCS:.c=.o)
endef
$(foreach target,$(BUILD),$(eval $(expand_build)))

%.d: %.c
	set -o pipefail && $(CC) -M $(CPPFLAGS) $< | sed '1s/:/ $@:/' >$@
include $(patsubst %.c,%.d,$(foreach target,$(BUILD),$($(target)_SRCS)))
