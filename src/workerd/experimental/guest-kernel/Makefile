CC ?= cc
# The library and any code that runs inside the guest must avoid a stack canary
# (needs TLS) and CET markers.
CFLAGS = -O2 -Wall -fno-stack-protector -fcf-protection=none -pthread

gk_test: gk.c gk_asm.S gk_test.c gk.h
	$(CC) $(CFLAGS) -o $@ gk.c gk_asm.S gk_test.c

# Same tests, but with the fault-path allocation guard armed: any malloc between
# GK_HANDLER_ENTER/LEAVE aborts. The tests exercise the fault path heavily (libc
# malloc inside the guest), so a clean pass proves the handler stays malloc-free.
gk_test_guard: gk.c gk_asm.S gk_test.c gk.h
	$(CC) $(CFLAGS) -DGK_GUARD_HANDLER_ALLOC \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc \
		-o $@ gk.c gk_asm.S gk_test.c

# Negative-test builds (never shipped): each removes one technique-B protection so the
# corresponding gk_test case fails, confirming the case actually tests the protection.
gk_test_neg: gk.c gk_asm.S gk_test.c gk.h
	$(CC) $(CFLAGS) -DGK_NEG_THREAD_WORDS -DGK_NEG_TURN_DIR -DGK_NEG_RESOLVE \
		-DGK_NEG_RESOLVE_OWNER -o $@ gk.c gk_asm.S gk_test.c

run: gk_test
	./gk_test

clean:
	rm -f gk_test gk_test_guard gk_test_neg

.PHONY: run clean
