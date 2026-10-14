# Guest-kernel isolation spike. x86-64 Linux with /dev/kvm only.
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
# m8 runs a compiled function inside the guest; keep the compiler from adding a
# stack canary (needs TLS) or CET endbr to that code.
M8_CFLAGS = -O2 -Wall -fno-stack-protector -fcf-protection=none

BINS = m1_hypercall m2_longmode m3_isolation m4_fault m5_exitcost \
       m6_switchcost m7_syscall m8_guest_enter m9_signal m10_ringtrap

all: $(BINS)

m8_guest_enter: m8_guest_enter.c
	$(CC) $(M8_CFLAGS) -o $@ $<

m9_signal: m9_signal.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

run: all
	@for b in $(BINS); do echo "== $$b =="; ./$$b || exit 1; echo; done

clean:
	rm -f $(BINS)

.PHONY: all run clean
