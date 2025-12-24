prefix ?= /usr/local
dest = $(DESTDIR)$(prefix)/blktests

all:
	$(MAKE) -C src all

clean:
	$(MAKE) -C src clean
	rm -f tags cscope.*

install:
	install -m755 -d $(dest)
	install check $(dest)
	cp -R tests common $(dest)
	$(MAKE) -C src dest=$(dest)/src install

tags: check_utility 
	@rm -f cscope.* tags
	find . -type f -print | grep -v -E ".out|.git|.md|.files|.cmd|tags|LICENSES|.el|.jinja2|.py|results" > cscope.files
	ctags -R --language-force=sh -L cscope.files --extras=+f
	cscope -bq -I/usr/include

check_utility:
	@command -v ctags &>/dev/null || (echo "Error: 'ctags' not found in PATH." && exit 1)
	@command -v cscope &>/dev/null || (echo "Error: 'cscope' not found in PATH." && exit 1)

# SC2119: "Use foo "$@" if function's $1 should mean script's $1". False
# positives on helpers like _init_scsi_debug.
SHELLCHECK_EXCLUDE := SC2119
NPROCS := $(shell nproc)

check:
	shellcheck -x -e $(SHELLCHECK_EXCLUDE) -f gcc check common/* \
		tests/*/rc tests/*/[0-9]*[0-9] src/*.sh
	shellcheck --exclude=$(SHELLCHECK_EXCLUDE),SC2154 --format=gcc new
	! grep TODO tests/*/rc tests/*/[0-9]*[0-9]
	! find -L -name '*.out' -perm /u=x+g=x+o=x -printf '%p is executable\n' | grep .

check-parallel:
	@echo "Running shellcheck with $(NPROCS) parallel jobs..."
	@ret=0; \
	find tests -type f -name '[0-9]*[0-9]' | \
		xargs -P $(NPROCS) -n 1 shellcheck -x -e $(SHELLCHECK_EXCLUDE) -f gcc || ret=1; \
	shellcheck -x -e $(SHELLCHECK_EXCLUDE) -f gcc check common/* tests/*/rc src/*.sh || ret=1; \
	shellcheck --exclude=$(SHELLCHECK_EXCLUDE),SC2154 --format=gcc new || ret=1; \
	grep TODO tests/*/rc tests/*/[0-9]*[0-9] && ret=1; \
	find -L -name '*.out' -perm /u=x+g=x+o=x -printf '%p is executable\n' | grep . && ret=1; \
	exit $$ret

.PHONY: all check check-parallel install tags check_utility
