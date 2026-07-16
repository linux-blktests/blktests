// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 0wnerD1ed */

#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/io_uring.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define NR_RECLAIM_PAGES 32
#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)

static size_t page_size;

static void *map(size_t size, int prot)
{
	void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		err(EXIT_FAILURE, "mmap");
	return p;
}

static uint64_t pagemap_pfn(int fd, const void *p)
{
	uint64_t entry;
	off_t offset = (uintptr_t)p / page_size * sizeof(entry);

	if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry))
		err(EXIT_FAILURE, "pagemap");
	if (!(entry & PAGEMAP_PRESENT) || !(entry & PAGEMAP_PFN_MASK))
		errx(EXIT_FAILURE, "pagemap PFN unavailable");
	return entry & PAGEMAP_PFN_MASK;
}

static int full_trim(const char *path)
{
	unsigned int block_size;
	unsigned char *p;
	ssize_t ret;
	int fd;

	fd = open(path, O_RDONLY | O_DIRECT);
	if (fd < 0)
		err(EXIT_FAILURE, "open %s", path);
	if (ioctl(fd, BLKSSZGET, &block_size))
		err(EXIT_FAILURE, "BLKSSZGET");

	p = map(2 * page_size, PROT_READ | PROT_WRITE);
	if (mprotect(p + page_size, page_size, PROT_NONE))
		err(EXIT_FAILURE, "mprotect");

	errno = 0;
	ret = pread(fd, p + page_size - 1, block_size, 0);
	if (ret == -1 && errno == EFAULT)
		return EXIT_SUCCESS;
	fprintf(stderr, "full-trim pread: ret=%zd errno=%d\n", ret, errno);
	return EXIT_FAILURE;
}

static void pin_cpu(void)
{
	cpu_set_t set;
	int cpu = sched_getcpu();

	if (cpu < 0)
		err(EXIT_FAILURE, "sched_getcpu");
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		err(EXIT_FAILURE, "sched_setaffinity");
}

static int bounce_read(const char *size, const char *path)
{
	struct io_uring_params params = {};
	void *first, *middle, *bad;
	struct iovec fixed, iov[3];
	uint64_t first_pfn;
	unsigned long block_size;
	unsigned int i;
	int memfd, pagemap_fd, ring_fd, fd;
	ssize_t ret;

	block_size = strtoul(size, NULL, 10);
	if (!block_size || block_size > UINT_MAX || block_size > page_size ||
	    (block_size & (block_size - 1)))
		errx(EXIT_FAILURE, "invalid block size");
	pin_cpu();

	memfd = syscall(SYS_memfd_create, "bio-bounce", 0);
	if (memfd < 0 || ftruncate(memfd, page_size))
		err(EXIT_FAILURE, "memfd");
	first = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd,
		     0);
	if (first == MAP_FAILED)
		err(EXIT_FAILURE, "mmap");
	middle = map(block_size, PROT_READ | PROT_WRITE);
	bad = map(block_size, PROT_NONE);
	memset(first, 'A', page_size);
	memset(middle, 'B', block_size);

	pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap_fd < 0)
		err(EXIT_FAILURE, "pagemap");
	first_pfn = pagemap_pfn(pagemap_fd, first);

	ring_fd = syscall(SYS_io_uring_setup, 2, &params);
	if (ring_fd < 0)
		err(EXIT_FAILURE, "io_uring_setup");
	fixed = (struct iovec){ first, page_size };
	if (syscall(SYS_io_uring_register, ring_fd, IORING_REGISTER_BUFFERS,
		    &fixed, 1) < 0)
		err(EXIT_FAILURE, "io_uring_register");

	iov[0] = (struct iovec){ first, 1 };
	iov[1] = (struct iovec){ middle, block_size };
	iov[2] = (struct iovec){ bad, block_size - 1 };
	fd = open(path, O_RDONLY | O_DIRECT);
	if (fd < 0)
		err(EXIT_FAILURE, "open %s", path);
	errno = 0;
	ret = preadv(fd, iov, 3, 0);
	if (ret != -1 || errno != EFAULT) {
		fprintf(stderr, "bounced preadv: ret=%zd errno=%d\n", ret,
			errno);
		return EXIT_FAILURE;
	}
	close(fd);
	munmap(first, page_size);
	close(memfd);

	for (i = 0; i < NR_RECLAIM_PAGES; i++) {
		unsigned char *p = map(page_size, PROT_READ | PROT_WRITE);

		*p = i;
		if (pagemap_pfn(pagemap_fd, p) == first_pfn) {
			fprintf(stderr, "fixed-buffer PFN was reclaimed\n");
			return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	long size = sysconf(_SC_PAGESIZE);

	if (size <= 0)
		errx(EXIT_FAILURE, "invalid page size");
	page_size = size;
	if (argc == 3 && !strcmp(argv[1], "full-trim"))
		return full_trim(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "bounce-read"))
		return bounce_read(argv[2], argv[3]);
	return EXIT_FAILURE;
}
