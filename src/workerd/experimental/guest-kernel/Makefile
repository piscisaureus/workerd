CC ?= cc
# The library and any code that runs inside the guest must avoid a stack canary
# (needs TLS) and, on x86-64, CET markers.
CFLAGS = -O2 -Wall -fno-stack-protector -pthread
ifeq ($(shell $(CC) -dumpmachine | cut -d- -f1),x86_64)
CFLAGS += -fcf-protection=none
endif

gk_test: gk.c gk_asm.S gk_test.c gk.h
	$(CC) $(CFLAGS) -o $@ gk.c gk_asm.S gk_test.c

# Same tests, but with the fault-path allocation guard armed: any malloc between
# GK_HANDLER_ENTER/LEAVE aborts. The tests exercise the fault path heavily (libc
# malloc inside the guest), so a clean pass proves the handler stays malloc-free.
gk_test_guard: gk.c gk_asm.S gk_test.c gk.h
	$(CC) $(CFLAGS) -DGK_GUARD_HANDLER_ALLOC \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc \
		-o $@ gk.c gk_asm.S gk_test.c

run: gk_test
	./gk_test

clean:
	rm -f gk_test gk_test_guard

.PHONY: run clean
