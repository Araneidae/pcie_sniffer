CFLAGS += -std=gnu99
CFLAGS += -O2 -Werror -Wall -Wextra
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers

CPPFLAGS += -D_GNU_SOURCE
CPPFLAGS += -D_FILE_OFFSET_BITS=64

LDFLAGS += -lpthread -lrt

all: fa_archiver archive_server live_server

clean:
	rm -f fa_archiver archive_server live_server
	rm -f *.o

fa_archiver: fa_archiver.c error.c

archive_server: archive_server.c error.c

live_server: live_server.c error.c

error.o: error.h


test: test.o error.o buffer.o sniffer.o disk_writer.o disk.o
prepare: prepare.o error.o disk.o
read: read.o error.o mask.o disk.o
dd: dd.o disk.o error.o
