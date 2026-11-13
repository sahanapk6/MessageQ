# SPDX-License-Identifier: GPL-2.0
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all: module tests

module:
	$(MAKE) -C driver KDIR=$(KDIR)

tests:
	$(MAKE) -C tests

# Load the module into the running kernel and run the tests (needs sudo).
check: all
	sudo insmod driver/msgring.ko ndevs=2
	tests/msgring_test /dev/msgring0; rc=$$?; \
	sudo rmmod msgring; exit $$rc

# Same tests inside a throwaway QEMU VM: no root, cannot harm the host.
qemu-test: module
	$(MAKE) -C tests clean
	$(MAKE) -C tests STATIC=1
	bash scripts/qemu-test.sh $(KVER)

clean:
	$(MAKE) -C driver KDIR=$(KDIR) clean
	$(MAKE) -C tests clean

.PHONY: all module tests check qemu-test clean
