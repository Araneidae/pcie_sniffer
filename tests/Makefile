CFLAGS += -std=gnu99
CFLAGS += -g
CFLAGS += -Werror
CFLAGS += -Wall -Wextra
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers
CFLAGS += -Wundef
CFLAGS += -Wshadow
CFLAGS += -Wcast-align
CFLAGS += -Wwrite-strings
CFLAGS += -Wredundant-decls
CFLAGS += -Wmissing-prototypes
CFLAGS += -Wmissing-declarations
CFLAGS += -Wstrict-prototypes
CFLAGS += -Wcast-qual

CPPFLAGS += -I..
LDFLAGS += -lm
LDFLAGS += -lrt

TESTS = backtrace timestamps testioctl sleep

%: %.c

default: $(TESTS)

clean:
	rm -f $(TESTS)

.PHONY: default clean
