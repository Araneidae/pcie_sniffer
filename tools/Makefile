# This Makefile is invoked by makefile after setting the working directory to
# the build directory.

all:    # Default target comes first!

CFLAGS += -std=gnu99
CFLAGS += -O2

CFLAGS += -Werror
CFLAGS += -Wall -Wextra
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers
CFLAGS += -Wundef
CFLAGS += -Wshadow
CFLAGS += -Wcast-align
CFLAGS += -Wwrite-strings
CFLAGS += -Wpointer-arith
CFLAGS += -Wredundant-decls
CFLAGS += -Wmissing-prototypes
CFLAGS += -Wmissing-declarations
CFLAGS += -Wstrict-prototypes

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
archiver_SRCS += socket_server.c

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
	$$(LINK.o) $$^ $$(LOADLIBES) $$(LDLIBS) -o $$@
endef
$(foreach target,$(BUILD),$(eval $(expand_build)))

%.d: %.c
	set -o pipefail && $(CC) -M $(CPPFLAGS) $< | sed '1s/:/ $@:/' >$@
include $(patsubst %.c,%.d,$(foreach target,$(BUILD),$($(target)_SRCS)))
