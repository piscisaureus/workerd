CC ?= cc
# The library and any code that runs inside the guest must avoid a stack canary
# (needs TLS) and CET markers.
CFLAGS = -O2 -Wall -fno-stack-protector -fcf-protection=none -pthread

gk_test: gk.c gk_asm.S gk_test.c gk.h
	$(CC) $(CFLAGS) -o $@ gk.c gk_asm.S gk_test.c

run: gk_test
	./gk_test

clean:
	rm -f gk_test

.PHONY: run clean
