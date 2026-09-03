CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic

.PHONY: all clean

all: tb330fu-folio-lid

tb330fu-folio-lid: tb330fu-folio-lid.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f tb330fu-folio-lid
