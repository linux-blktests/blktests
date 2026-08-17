// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Meta Platforms, Inc.  All Rights Reserved.
 *
 * Description: test direct-io memory alignment offsets
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>

#define power_of_2(x) ((x) && !((x) & ((x) - 1)))
#define err_errno(exit_code, ...)		\
	do {					\
		errno = (exit_code);		\
		err(EXIT_FAILURE, __VA_ARGS__); \
	} while (0)

static unsigned long logical_block_size;
static unsigned long dma_alignment;
static unsigned long virt_boundary;
static unsigned long max_segments;
static unsigned long max_bytes;
static size_t buf_size;
static long pagesize;
static void *out_buf;
static void *in_buf;
static int test_fd;

static void init_args(char **argv)
{
	test_fd = open(argv[1], O_RDWR | O_TRUNC | O_DIRECT);
	if (test_fd < 0)
		err(EXIT_FAILURE, "%s: failed to open %s", __func__, argv[1]);

	max_segments = strtoul(argv[2], NULL, 0);
	max_bytes = strtoul(argv[3], NULL, 0) * 1024;
	dma_alignment = strtoul(argv[4], NULL, 0) + 1;
	virt_boundary = strtoul(argv[5], NULL, 0) + 1;
	logical_block_size = strtoul(argv[6], NULL, 0);

	if (!power_of_2(virt_boundary) ||
	    !power_of_2(dma_alignment) ||
	    !power_of_2(logical_block_size)) {
		errno = EINVAL;
		err(EXIT_FAILURE, "%s: bad parameters", __func__);
	}

	if (virt_boundary > 1 && virt_boundary < logical_block_size) {
		errno = EINVAL;
		err(EXIT_FAILURE, "%s: virt_boundary:%lu logical_block_size:%lu", __func__,
			virt_boundary, logical_block_size);
	}

	if (dma_alignment > logical_block_size) {
		errno = EINVAL;
		err(EXIT_FAILURE, "%s: dma_alignment:%lu logical_block_size:%lu", __func__,
			dma_alignment, logical_block_size);
	}

	if (max_segments > 4096)
		max_segments = 4096;
	if (max_bytes > 16384 * 1024)
		max_bytes = 16384 * 1024;
	if (max_bytes & (logical_block_size - 1))
		max_bytes -= max_bytes & (logical_block_size - 1);

	pagesize = sysconf(_SC_PAGE_SIZE);
}

static void init_buffers()
{
	unsigned long lb_mask = logical_block_size - 1;
	int fd, ret;
	unsigned long long dev_bytes;

	buf_size = max_bytes * max_segments / 2;
	if (buf_size < logical_block_size * max_segments)
		err_errno(EINVAL, "%s: logical block size is too big", __func__);

	if (buf_size < logical_block_size * 1024 * 4)
		buf_size = logical_block_size * 1024 * 4;

	if (buf_size & lb_mask)
		buf_size = (buf_size + lb_mask) & ~(lb_mask);

	ret = ioctl(test_fd, BLKGETSIZE64, &dev_bytes);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: ioctl BLKGETSIZE64 failed", __func__);

	if (dev_bytes < buf_size)
		buf_size = dev_bytes;

	ret = posix_memalign((void **)&in_buf, pagesize, buf_size);
	if (ret)
		err_errno(EINVAL, "%s: failed to allocate in-buf", __func__);

	ret = posix_memalign((void **)&out_buf, pagesize, buf_size);
	if (ret)
		err_errno(EINVAL, "%s: failed to allocate out-buf", __func__);

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		err_errno(EINVAL, "%s: failed to open urandom", __func__);

	ret = read(fd, out_buf, buf_size);
	if (ret < 0)
		err_errno(EINVAL, "%s: failed to randomize output buffer",
			  __func__);

	close(fd);
}

static void __compare(void *a, void *b, size_t size, const char *test)
{
	size_t i;

	if (!memcmp(a, b, size))
		return;
	for (i = 0; i < size; i++)
		if (((uint8_t *)a)[i] != ((uint8_t *)b)[i])
			break;
	err_errno(EIO, "%s: data corruption at offset %zd/%zd", test, i, size);
}
#define compare(a, b, size) __compare(a, b, size, __func__)

/*
 * Test using page aligned buffers, single source
 *
 * Total size is aligned to a logical block size and exceeds the max transfer
 * size as well as the max segments. This should test the kernel's split bio
 * construction and bio splitting for exceeding these limits.
 */
static void test_full_size_aligned()
{
	int ret;

	memset(in_buf, 0, buf_size);
	ret = pwrite(test_fd, out_buf, buf_size, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to write buf", __func__);

	ret = pread(test_fd, in_buf, buf_size, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	compare(out_buf, in_buf, buf_size);
}

/*
 * Test using dma aligned buffers, single source
 *
 * This tests the kernel's dio memory alignment
 */
static void test_dma_aligned()
{
	int ret;

	memset(in_buf, 0, buf_size);
	ret = pwrite(test_fd, out_buf + dma_alignment, max_bytes, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to write buf", __func__);

	ret = pread(test_fd, in_buf + dma_alignment, max_bytes, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	compare(out_buf + dma_alignment, in_buf + dma_alignment, max_bytes);
}

/*
 * Test using page aligned buffers + logicaly block sized vectored source
 *
 * This tests discontiguous vectored sources
 */
static void test_page_aligned_vectors()
{
	const int vecs = 4;

	int i, ret, offset;
	struct iovec iov[vecs];

	memset(in_buf, 0, buf_size);
	for (i = 0; i < vecs; i++) {
		offset = logical_block_size * i * 4;
		iov[i].iov_base = out_buf + offset;
		iov[i].iov_len = logical_block_size * 2;
	}

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to write buf", __func__);

	for (i = 0; i < vecs; i++) {
		offset = logical_block_size * i * 4;
		iov[i].iov_base = in_buf + offset;
		iov[i].iov_len = logical_block_size * 2;
	}

	ret = preadv(test_fd, iov, vecs, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	for (i = 0; i < vecs; i++) {
		offset = logical_block_size * i * 4;
		compare(in_buf + offset, out_buf + offset, logical_block_size * 2);
	}
}

/*
 * Test using dma aligned buffers, vectored source
 *
 * This tests discontiguous vectored sources with incrementing dma aligned
 * offsets
 */
static void test_dma_aligned_vectors()
{
	const int vecs = 4;

	int i, ret, offset;
	struct iovec iov[vecs];

	memset(in_buf, 0, buf_size);
	for (i = 0; i < vecs; i++) {
		offset = logical_block_size * i * 8 + dma_alignment * (i + 1);
		iov[i].iov_base = out_buf + offset;
		iov[i].iov_len = logical_block_size * 2;
	}

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to write buf", __func__);

	for (i = 0; i < vecs; i++) {
		offset = logical_block_size * i * 8 + dma_alignment * (i + 1);
		iov[i].iov_base = in_buf + offset;
		iov[i].iov_len = logical_block_size * 2;
	}

	ret = preadv(test_fd, iov, vecs, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	for (i = 0; i < vecs; i++) {
		offset = logical_block_size * i * 8 + dma_alignment * (i + 1);
		compare(in_buf + offset, out_buf + offset, logical_block_size * 2);
	}
}

/*
 * Test vectored read with a total size aligned to a block, but some individual
 * vectors will not be aligned to to the block size.
 *
 * All the middle vectors start and end on page boundaries which should
 * satisfy any virt_boundary condition. This test will fail prior to kernel
 * 6.18.
 */
static void test_unaligned_page_vectors()
{
	const int vecs = 4;

	int i, ret, offset, mult;
	struct iovec iov[vecs];
	bool should_fail = true;

	i = 0;
	memset(in_buf, 0, buf_size);
	mult = pagesize / logical_block_size;
	if (mult < 2)
		mult = 2;

	offset = pagesize - (logical_block_size / 4);
	if (offset & (dma_alignment - 1))
		offset = pagesize - dma_alignment;

	iov[i].iov_base = out_buf + offset;
	iov[i].iov_len = pagesize - offset;

	for (i = 1; i < vecs - 1; i++) {
		offset = logical_block_size * i * 8 * mult;
		iov[i].iov_base = out_buf + offset;
		iov[i].iov_len = logical_block_size * mult;
	}

	offset = logical_block_size * i * 8 * mult;
	iov[i].iov_base = out_buf + offset;
	iov[i].iov_len = logical_block_size * mult - iov[0].iov_len;

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0) {
		if (should_fail)
			return;
		err(EXIT_FAILURE, "%s: failed to write buf", __func__);
	}

	i = 0;
	offset = pagesize - (logical_block_size / 4);
	if (offset & (dma_alignment - 1))
		offset = pagesize - dma_alignment;

	iov[i].iov_base = in_buf + offset;
	iov[i].iov_len = pagesize - offset;

	for (i = 1; i < vecs - 1; i++) {
		offset = logical_block_size * i * 8 * mult;
		iov[i].iov_base = in_buf + offset;
		iov[i].iov_len = logical_block_size * mult;
	}

	offset = logical_block_size * i * 8 * mult;
	iov[i].iov_base = in_buf + offset;
	iov[i].iov_len = logical_block_size * mult - iov[0].iov_len;

	ret = preadv(test_fd, iov, vecs, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	i = 0;
	offset = pagesize - (logical_block_size / 4);
	if (offset & (dma_alignment - 1))
		offset = pagesize - dma_alignment;

	compare(in_buf + offset, out_buf + offset, iov[i].iov_len);
	for (i = 1; i < vecs - 1; i++) {
		offset = logical_block_size * i * 8 * mult;
		compare(in_buf + offset, out_buf + offset, iov[i].iov_len);
	}
	offset = logical_block_size * i * 8 * mult;
	compare(in_buf + offset, out_buf + offset, iov[i].iov_len);
}

/*
 * Total size is a logical block size multiple, but none of the vectors are.
 *
 * Total vectors will be less than the max. The vectors will be dma aligned. If
 * a virtual boundary exists, this should fail, otherwise it should succceed on
 * kernels 6.18 and newer.
 */
static void test_unaligned_vectors()
{
	const int vecs = 4;

	struct iovec iov[vecs];
	int i, ret, offset;

	memset(in_buf, 0, buf_size);
	for (i = 0; i < vecs; i++) {
		offset = logical_block_size * i * 8;
		iov[i].iov_base = out_buf + offset;
		iov[i].iov_len = logical_block_size / 2;
	}

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0)
		return;

	for (i = 0; i < vecs; i++) {
		offset = logical_block_size * i * 8;
		iov[i].iov_base = in_buf + offset;
		iov[i].iov_len = logical_block_size / 2;
	}

	ret = preadv(test_fd, iov, vecs, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	for (i = 0; i < vecs; i++) {
		offset = logical_block_size * i * 8;
		compare(in_buf + offset, out_buf + offset, logical_block_size / 2);
	}
}

/*
 * Provide an invalid iov_base at the beginning to test the kernel catching it
 * while building a bio.
 */
static void test_invalid_starting_addr()
{
	const int vecs = 4;

	int i, ret, offset;
	struct iovec iov[vecs];

	i = 0;
	iov[i].iov_base = 0;
	iov[i].iov_len = logical_block_size;

	for (i = 1; i < vecs; i++) {
		offset = logical_block_size * i * 8;
		iov[i].iov_base = out_buf + offset;
		iov[i].iov_len = logical_block_size;
	}

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0)
		return;

	err_errno(ENOTSUP,
		  "%s: write buf unexpectedly succeeded with NULL address ret:%d",
		  __func__, ret);
}

/*
 * Provide an invalid iov_base in the middle to test the kernel catching it
 * while building split bios. Ensure it is split by sending enough vectors to
 * exceed bio's MAX_VEC; this should cause part of the io to dispatch.
 */
static void test_invalid_middle_addr()
{
	const int vecs = 1024;

	int i, ret, offset;
	struct iovec iov[vecs];

	for (i = 0; i < vecs / 2 + 1; i++) {
		offset = logical_block_size * i * 2;
		iov[i].iov_base = out_buf + offset;
		iov[i].iov_len = logical_block_size;
	}

	offset = logical_block_size * i * 2;
	iov[i].iov_base = 0;
	iov[i].iov_len = logical_block_size;

	for (++i; i < vecs; i++) {
		offset = logical_block_size * i * 2;
		iov[i].iov_base = out_buf + offset;
		iov[i].iov_len = logical_block_size;
	}

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0)
		return;

	err_errno(ENOTSUP,
		  "%s: write buf unexpectedly succeeded with NULL address ret:%d",
		  __func__, ret);
}

/*
 * Test with an invalid DMA address. Should get caught early when splitting. If
 * the device supports byte aligned memory (which is unusual), then this should
 * be successful.
 */
static void test_invalid_dma_alignment()
{
	int ret, offset;
	size_t size;
	bool should_fail = dma_alignment > 1;

	memset(in_buf, 0, buf_size);
	offset = 2 * dma_alignment - 1;
	size = logical_block_size * 256;
	ret = pwrite(test_fd, out_buf + offset, size, 0);
	if (ret < 0) {
		if (should_fail)
			return;
		err(EXIT_FAILURE, "%s: failed to write buf", __func__);
	}

	if (should_fail)
		err_errno(ENOTSUP,
			  "%s: write buf unexpectedly succeeded with invalid DMA offset address, ret:%d",
			  __func__, ret);

	ret = pread(test_fd, in_buf + offset, size, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	compare(out_buf + offset, in_buf + offset, size);
}

/*
 * Test with invalid DMA alignment in the middle. This should get split with
 * the first part being dispatched, and the 2nd one failing without dispatch.
 */
static void test_invalid_dma_vector_alignment()
{
	const int vecs = 5;

	bool should_fail = dma_alignment > 1;
	struct iovec iov[vecs];
	int ret, offset;

	offset = dma_alignment * 2 - 1;
	memset(in_buf, 0, buf_size);

	iov[0].iov_base = out_buf;
	iov[0].iov_len = max_bytes;

	iov[1].iov_base = out_buf + max_bytes * 2;
	iov[1].iov_len = max_bytes;

	iov[2].iov_base = out_buf + max_bytes * 4 + offset;
	iov[2].iov_len = max_bytes;

	iov[3].iov_base = out_buf + max_bytes * 6;
	iov[3].iov_len = max_bytes;

	iov[4].iov_base = out_buf + max_bytes * 8;
	iov[4].iov_len = max_bytes;

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0) {
		if (should_fail)
			return;
		err(EXIT_FAILURE, "%s: failed to write buf", __func__);
	}
	if (should_fail)
		err_errno(ENOTSUP,
			  "%s: write buf unexpectedly succeeded with invalid DMA offset address ret:%d",
			  __func__, ret);

	iov[0].iov_base = in_buf;
	iov[0].iov_len = max_bytes;

	iov[1].iov_base = in_buf + max_bytes * 2;
	iov[1].iov_len = max_bytes;

	iov[2].iov_base = in_buf + max_bytes * 4 + offset;
	iov[2].iov_len = max_bytes;

	iov[3].iov_base = in_buf + max_bytes * 6;
	iov[3].iov_len = max_bytes;

	iov[4].iov_base = in_buf + max_bytes * 8;
	iov[4].iov_len = max_bytes;

	ret = preadv(test_fd, iov, vecs, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	compare(out_buf, in_buf, max_bytes);
	compare(out_buf + max_bytes * 2, in_buf + max_bytes * 2, max_bytes);
	compare(out_buf + max_bytes * 4 + offset, in_buf + max_bytes * 4 + offset, max_bytes);
	compare(out_buf + max_bytes * 6, in_buf + max_bytes * 6, max_bytes);
	compare(out_buf + max_bytes * 8, in_buf + max_bytes * 8, max_bytes);
}

/*
 * Test a bunch of small vectors if the device dma alignemnt allows it. We'll
 * try to force a MAX_IOV split that can't form a valid IO so expect a failure.
 */
static void test_max_vector_limits()
{
	const int vecs = 320;

	int ret, i, offset, iovpb, iov_size;
	bool should_fail = true;
	struct iovec iov[vecs];

	memset(in_buf, 0, buf_size);
	iovpb = logical_block_size / dma_alignment;
	iov_size = logical_block_size / iovpb;

	if ((pagesize  / iov_size) < 256 &&
	    iov_size >= virt_boundary)
		should_fail = false;

	for (i = 0; i < vecs; i++) {
		offset = i * iov_size * 2;
		iov[i].iov_base = out_buf + offset;
		iov[i].iov_len = iov_size;
	}

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0) {
		if (should_fail)
			return;
		err(EXIT_FAILURE, "%s: failed to write buf", __func__);
	}

	if (should_fail)
		err_errno(ENOTSUP,
			  "%s: write buf unexpectedly succeeded with excess vectors ret:%d",
			  __func__, ret);

	for (i = 0; i < vecs; i++) {
		offset = i * iov_size * 2;
		iov[i].iov_base = in_buf + offset;
		iov[i].iov_len = iov_size;
	}

	ret = preadv(test_fd, iov, vecs, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	for (i = 0; i < vecs; i++) {
		offset = i * iov_size * 2;
		compare(in_buf + offset, out_buf + offset, logical_block_size / 2);
	}
}

/*
 * Start with a valid vector that can be split into a dispatched IO, but poison
 * the rest with an invalid DMA offset testing the kernel's late catch.
 */
static void test_invalid_dma_vector_alignment_large()
{
	const int vecs = 4;

	struct iovec iov[vecs];
	int i, ret;

	i = 0;
	iov[i].iov_base = out_buf;
	iov[i].iov_len = max_bytes - logical_block_size;

	i++;
	iov[i].iov_base = out_buf + max_bytes + logical_block_size;
	iov[i].iov_len = logical_block_size;

	i++;
	iov[i].iov_base = iov[1].iov_base + pagesize * 2 + (dma_alignment - 1);
	iov[i].iov_len = logical_block_size;

	i++;
	iov[i].iov_base = out_buf + max_bytes * 8;
	iov[i].iov_len = logical_block_size;

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0)
		return;

	err_errno(ENOTSUP,
		  "%s: write buf unexpectedly succeeded with NULL address ret:%d",
		  __func__, ret);
}

/*
 * Total size is block aligned, addresses are dma aligned, but invidual vector
 * sizes may not be dma aligned. If device has byte sized dma alignment, this
 * should succeed. If not, part of this should get dispatched, and the other
 * part should fail.
 */
static void test_invalid_dma_vector_length()
{
	const int vecs = 4;

	bool should_fail = dma_alignment > 1;
	struct iovec iov[vecs];
	int ret;

	iov[0].iov_base = out_buf;
	iov[0].iov_len = max_bytes * 2 - max_bytes / 2;

	iov[1].iov_base = out_buf + max_bytes * 4;
	iov[1].iov_len = logical_block_size * 2 - (dma_alignment + 1);

	iov[2].iov_base = out_buf + max_bytes * 8;
	iov[2].iov_len = logical_block_size * 2 + (dma_alignment + 1);

	iov[3].iov_base = out_buf + max_bytes * 12;
	iov[3].iov_len = max_bytes - max_bytes / 2;

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0) {
		if (should_fail)
			return;
		err(EXIT_FAILURE, "%s: failed to write buf", __func__);
	}

	if (should_fail)
		err_errno(ENOTSUP,
			  "%s: write buf unexpectedly succeeded with invalid DMA offset address ret:%d",
			  __func__, ret);

	iov[0].iov_base = in_buf;
	iov[0].iov_len = max_bytes * 2 - max_bytes / 2;

	iov[1].iov_base = in_buf + max_bytes * 4;
	iov[1].iov_len = logical_block_size * 2 - (dma_alignment + 1);

	iov[2].iov_base = in_buf + max_bytes * 8;
	iov[2].iov_len = logical_block_size * 2 + (dma_alignment + 1);

	iov[3].iov_base = in_buf + max_bytes * 12;
	iov[3].iov_len = max_bytes - max_bytes / 2;

	ret = pwritev(test_fd, iov, vecs, 0);
	if (ret < 0)
		err(EXIT_FAILURE, "%s: failed to read buf", __func__);

	compare(out_buf, in_buf, iov[0].iov_len);
	compare(out_buf + max_bytes * 4, in_buf + max_bytes * 4, iov[1].iov_len);
	compare(out_buf + max_bytes * 8, in_buf + max_bytes * 8, iov[2].iov_len);
	compare(out_buf + max_bytes * 12, in_buf + max_bytes * 12, iov[3].iov_len);
}

static void run_tests()
{
	test_full_size_aligned();
	test_dma_aligned();
	test_page_aligned_vectors();
	test_dma_aligned_vectors();
	test_unaligned_page_vectors();
	test_unaligned_vectors();
	test_invalid_starting_addr();
	test_invalid_middle_addr();
	test_invalid_dma_alignment();
	test_invalid_dma_vector_alignment();
	test_max_vector_limits();
	test_invalid_dma_vector_alignment_large();
	test_invalid_dma_vector_length();
}

/* ./$prog-name file */
int main(int argc, char **argv)
{
	if (argc < 2)
		errx(EINVAL, "expect argments: file");

	init_args(argv);
	init_buffers();
	run_tests();
	close(test_fd);
	free(out_buf);
	free(in_buf);

	return 0;
}
