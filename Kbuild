ifneq ($(shell hostname -s),pc0046)
$(error This make file needs to be run on pc46, at least for now)
endif

EXTRA_CFLAGS += -O2 -Werror -Wextra
EXTRA_CFLAGS += -Wno-declaration-after-statement -Wno-unused-parameter
EXTRA_CFLAGS += -Wno-missing-field-initializers

obj-m += fa_sniffer.o
