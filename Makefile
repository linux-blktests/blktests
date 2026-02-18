prefix ?= /usr/local
dest = $(DESTDIR)$(prefix)/blktests

all:
	$(MAKE) -C src all

clean:
	$(MAKE) -C src clean

install:
	install -m755 -d $(dest)
	install check $(dest)
	cp -R tests common $(dest)
	$(MAKE) -C src dest=$(dest)/src install

# SC2119: "Use foo "$@" if function's $1 should mean script's $1". False
# positives on helpers like _init_scsi_debug.
SHELLCHECK_EXCLUDE := SC2119
NPROCS := $(shell nproc)

check:
	shellcheck -x -e $(SHELLCHECK_EXCLUDE) -f gcc check common/* \
		tests/*/rc tests/*/[0-9]*[0-9] src/*.sh contrib/run_ci
	shellcheck --exclude=$(SHELLCHECK_EXCLUDE),SC2154 --format=gcc new
	! grep TODO tests/*/rc tests/*/[0-9]*[0-9]
	! find -L -name '*.out' -perm /u=x+g=x+o=x -printf '%p is executable\n' | grep .

check-parallel:
	@echo "Running shellcheck with $(NPROCS) parallel jobs..."
	@ret=0; \
	find tests -type f -name '[0-9]*[0-9]' | \
		xargs -P $(NPROCS) -n 1 shellcheck -x -e $(SHELLCHECK_EXCLUDE) -f gcc || ret=1; \
	shellcheck -x -e $(SHELLCHECK_EXCLUDE) -f gcc check common/* tests/*/rc src/*.sh contrib/run_ci || ret=1; \
	shellcheck --exclude=$(SHELLCHECK_EXCLUDE),SC2154 --format=gcc new || ret=1; \
	grep TODO tests/*/rc tests/*/[0-9]*[0-9] && ret=1; \
	find -L -name '*.out' -perm /u=x+g=x+o=x -printf '%p is executable\n' | grep . && ret=1; \
	exit $$ret

.PHONY: all check check-parallel install
