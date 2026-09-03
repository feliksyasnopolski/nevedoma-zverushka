CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDFLAGS ?=

.PHONY: all policy clean

POLICY = generated_ueventd_policy.c generated_ueventd_policy.h

all: tb330fu-devd

# The checked-in policy is the normal build input. Regeneration is explicit
# because its stock-derived inputs intentionally live outside this repository.
policy:
	test -n "$(SYSTEM_UEVENTD)" || (echo "set SYSTEM_UEVENTD=/path/to/ueventd.rc" >&2; exit 2)
	test -n "$(POLICY_TSV)" || (echo "set POLICY_TSV=/path/to/ueventd-policy.tsv" >&2; exit 2)
	python3 gen_ueventd_policy.py --system "$(SYSTEM_UEVENTD)" --tsv "$(POLICY_TSV)" --out-dir . --self-test

tb330fu-devd: tb330fu-devd.c generated_ueventd_policy.c generated_ueventd_policy.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tb330fu-devd.c generated_ueventd_policy.c

clean:
	rm -f tb330fu-devd
