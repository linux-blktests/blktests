// SPDX-License-Identifier: GPL-3.0+
/*
 * Copyright (C) 2026 Tal Zussman
 *
 * Check that a write to a block device through a shared mapping reaches the
 * disk when the block size is below the folio size.
 *
 * Write a known pattern with O_DIRECT, store a different one through an mmap()
 * of the same range, msync() and fsync(), then read the range back with
 * O_DIRECT and compare.
 *
 * With more than one block per folio, writeback tracks dirty state per block
 * rather than per folio, so dirtying through a mapping has to keep the
 * per-block state in sync. If it does not, writeback finds nothing dirty,
 * submits no I/O and clears PG_dirty, and the store is silently lost. With a
 * single block per folio there is no per-block state to get out of sync, so
 * the block size must be below the folio size for this to test anything.
 *
 * usage: mmap-loss <blockdev> <blocksize>
 *
 * exit:  0 = data intact
 *        1 = setup error
 *        2 = data lost
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/fs.h>

#define EXIT_LOST	2

int main(int argc, char **argv)
{
	long pgsz;
	char *buf;
	char *map;
	int want_bs;
	int bs, fd, i;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <blockdev> <blocksize>\n", argv[0]);
		return EXIT_FAILURE;
	}

	want_bs = atoi(argv[2]);

	pgsz = sysconf(_SC_PAGESIZE);
	if (pgsz < 0) {
		perror("sysconf");
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDWR | O_DIRECT);
	if (fd < 0) {
		perror("open");
		return EXIT_FAILURE;
	}

	if (ioctl(fd, BLKBSZSET, &want_bs)) {
		perror("BLKBSZSET");
		return EXIT_FAILURE;
	}

	if (ioctl(fd, BLKBSZGET, &bs)) {
		perror("BLKBSZGET");
		return EXIT_FAILURE;
	}
	printf("block size: %d, page size: %ld\n", bs, pgsz);

	/*
	 * With one block per folio there is no iomap_folio_state to get out of
	 * sync, so the bug cannot be observed.
	 */
	if (bs >= pgsz) {
		fprintf(stderr, "block size %d is not below the page size\n",
			bs);
		return EXIT_FAILURE;
	}

	if (posix_memalign((void **)&buf, pgsz, pgsz)) {
		perror("posix_memalign");
		return EXIT_FAILURE;
	}

	/* put a known pattern on disk, bypassing the page cache */
	memset(buf, 'A', pgsz);
	if (pwrite(fd, buf, pgsz, 0) != pgsz) {
		perror("pwrite");
		return EXIT_FAILURE;
	}

	/* fault the folio in, then dirty it through the mapping */
	map = mmap(NULL, pgsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		return EXIT_FAILURE;
	}

	if (map[0] != 'A') {
		fprintf(stderr, "unexpected content '%c'\n", map[0]);
		return EXIT_FAILURE;
	}

	memset(map, 'B', pgsz);

	/* (try to) sync the mapping to disk */
	if (msync(map, pgsz, MS_SYNC)) {
		perror("msync");
		return EXIT_FAILURE;
	}

	if (fsync(fd)) {
		perror("fsync");
		return EXIT_FAILURE;
	}

	/* unmap to avoid any interactions between buffered I/O and direct I/O */
	if (munmap(map, pgsz)) {
		perror("munmap");
		return EXIT_FAILURE;
	}

	/* read back from disk with direct I/O */
	memset(buf, 0, pgsz);
	if (pread(fd, buf, pgsz, 0) != pgsz) {
		perror("pread");
		return EXIT_FAILURE;
	}

	for (i = 0; i < pgsz; i++) {
		if (buf[i] != 'B') {
			printf("byte %d on disk is '%c', mmap write was lost\n",
			       i, buf[i]);
			return EXIT_LOST;
		}
	}

	printf("mmap write reached disk\n");
	return EXIT_SUCCESS;
}
