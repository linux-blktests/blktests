H := \#

HAVE_C_HEADER = $(shell if echo "$(H)include <$(1)>" |		\
		$(CC) $(CFLAGS) -E - > /dev/null 2>&1; then echo "$(2)";	\
		else echo "$(3)"; fi)

HAVE_C_MACRO = $(shell if echo "$(H)include <$(1)>" |	\
		$(CC) $(CFLAGS) -E - 2>&1 /dev/null | grep $(2) > /dev/null 2>&1; \
		then echo 1;else echo 0; fi)

C_TARGETS := \
	dio-offsets \
	loblksize \
	loop_change_fd \
	loop_get_status_null \
	mount_clear_sock \
	nvme-passthrough-meta \
	ioctl-lbmd-query \
	nbdsetsize \
	openclose \
	sg/dxfer-from-dev \
	sg/syzkaller1 \
	zbdioctl

C_URING_TARGETS := metadata \
	nvme-passthru-admin-uring
C_UBLK_TARGETS := miniublk

HAVE_LIBURING := $(call HAVE_C_MACRO,liburing.h,IORING_OP_URING_CMD)
HAVE_UBLK_HEADER := $(call HAVE_C_HEADER,linux/ublk_cmd.h,1)

CXX_TARGETS := \
	discontiguous-io

TARGETS := $(C_TARGETS) $(CXX_TARGETS)

ifeq ($(HAVE_UBLK_HEADER), 1)
C_URING_TARGETS += $(C_UBLK_TARGETS)
else
$(info Skip $(C_UBLK_TARGETS) build due to missing kernel header(v6.0+))
endif

ifeq ($(HAVE_LIBURING), 1)
TARGETS += $(C_URING_TARGETS)
else
$(info Skip $(C_URING_TARGETS) build due to missing liburing(2.2+))
endif

CONFIG_DEFS := $(call HAVE_C_HEADER,linux/blkzoned.h,-DHAVE_LINUX_BLKZONED_H)

override CFLAGS   := -O2 -Wall -Wshadow $(CFLAGS) $(CONFIG_DEFS)
override CXXFLAGS := -O2 -std=c++11 -Wall -Wextra -Wshadow -Wno-sign-compare \
		     -Werror $(CXXFLAGS) $(CONFIG_DEFS)
URING_FLAGS :=  -D_GNU_SOURCE
URING_LIBS := -lpthread -luring
LDFLAGS ?=

all: $(TARGETS)

clean:
	rm -f $(TARGETS)

install: $(TARGETS)
	install -m755 -d $(dest)
	install $(TARGETS) $(dest)

$(C_TARGETS): %: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(CXX_TARGETS): %: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

$(C_URING_TARGETS): %: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) $(URING_FLAGS) -o $@ $^ $(URING_LIBS)

.PHONY: all clean install
