// SPDX-License-Identifier: GPL-3.0+
/*
 * Copyright (C) 2026 Tal Zussman
 *
 * Check that a RWF_ATOMIC direct write to a block device is all or nothing
 * when only part of its buffer can be pinned.
 *
 * Fill the first two pages of <blockdev> with one byte value, then write two
 * pages of another value with RWF_ATOMIC from a buffer whose last page is
 * PROT_NONE, so that pinning stops one page short. The write has to fail
 * with EINVAL and leave the old bytes on the device. The device has to
 * support an atomic write of two pages, which is checked with statx().
 *
 * usage: atomic-write-untorn <blockdev>
 *
 * exit:  0 = the write failed with EINVAL and the device is unchanged
 *        1 = it did not, or a setup error
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define OLD	'A'
#define NEW	'B'

static bool bdev_can_atomic_write_for_length(int fd, size_t len)
{
	struct statx stx = { 0 };

	if (statx(fd, "", AT_EMPTY_PATH, STATX_WRITE_ATOMIC, &stx)) {
		perror("statx");
		return false;
	}
	if (!(stx.stx_mask & STATX_WRITE_ATOMIC) ||
	    !(stx.stx_attributes & STATX_ATTR_WRITE_ATOMIC)) {
		fprintf(stderr, "device does not support atomic writes\n");
		return false;
	}
	if (len < stx.stx_atomic_write_unit_min ||
	    len > stx.stx_atomic_write_unit_max) {
		fprintf(stderr, "device cannot write %zu bytes atomically\n",
			len);
		return false;
	}
	return true;
}

/*
 * Read the range back past the page cache and return how many of its bytes
 * hold val, or -1 on error.
 */
static ssize_t compare_bytes(int fd, char *buf, size_t len, char val)
{
	size_t i, n = 0;

	if (pread(fd, buf, len, 0) != (ssize_t)len) {
		perror("pread");
		return -1;
	}
	for (i = 0; i < len; i++)
		if (buf[i] == val)
			n++;
	return n;
}

int main(int argc, char **argv)
{
	size_t len, pgsz;
	struct iovec iov;
	char *buf;
	ssize_t ret, n;
	int fd, err, status = EXIT_FAILURE;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <blockdev>\n", argv[0]);
		return EXIT_FAILURE;
	}

	pgsz = sysconf(_SC_PAGESIZE);
	len = 2 * pgsz;

	fd = open(argv[1], O_RDWR | O_DIRECT);
	if (fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}

	if (!bdev_can_atomic_write_for_length(fd, len))
		goto out_close;

	buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) {
		perror("mmap");
		goto out_close;
	}

	memset(buf, OLD, len);
	if (pwrite(fd, buf, len, 0) != (ssize_t)len) {
		perror("pwrite");
		goto out_unmap;
	}
	n = compare_bytes(fd, buf, len, OLD);
	if (n != (ssize_t)len) {
		if (n >= 0)
			fprintf(stderr, "old contents did not read back\n");
		goto out_unmap;
	}

	/* Make the last page of the new contents unreadable. */
	memset(buf, NEW, len);
	if (mprotect(buf + len - pgsz, pgsz, PROT_NONE)) {
		perror("mprotect");
		goto out_unmap;
	}

	iov.iov_base = buf;
	iov.iov_len = len;
	ret = pwritev2(fd, &iov, 1, 0, RWF_ATOMIC);
	err = errno;

	if (mprotect(buf + len - pgsz, pgsz, PROT_READ | PROT_WRITE)) {
		perror("mprotect");
		goto out_unmap;
	}
	n = compare_bytes(fd, buf, len, OLD);
	if (n < 0)
		goto out_unmap;

	if (n == 0) {
		fprintf(stderr, "atomic write went through despite the unreadable page\n");
	} else if (n != (ssize_t)len) {
		fprintf(stderr, "atomic write was torn: %zu of %zu bytes written\n",
			len - n, len);
	} else if (ret >= 0) {
		fprintf(stderr, "pwritev2(RWF_ATOMIC) returned %zd but left the device unchanged\n",
			ret);
	} else if (err != EINVAL) {
		fprintf(stderr, "pwritev2(RWF_ATOMIC) failed with %s, not EINVAL\n",
			strerror(err));
	} else {
		printf("atomic write failed with EINVAL and left the device unchanged, as expected\n");
		status = EXIT_SUCCESS;
	}

out_unmap:
	munmap(buf, len);
out_close:
	close(fd);
	return status;
}
