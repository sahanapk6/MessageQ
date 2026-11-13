# SPDX-License-Identifier: GPL-2.0
CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Werror -pthread -I../include
# STATIC=1 builds a self-contained binary for the QEMU initramfs.
LDFLAGS += $(if $(STATIC),-static)

msgring_test: msgring_test.c ../include/msgring.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f msgring_test

.PHONY: clean
