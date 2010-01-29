CFLAGS += -std=gnu99
CFLAGS += -O2 -Werror -Wall -Wextra
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers

CPPFLAGS += -D_GNU_SOURCE

LDFLAGS += -lpthread

all: fa_archiver

clean:
	rm -f fa_archiver

fa_archiver: fa_archiver.c error.c

