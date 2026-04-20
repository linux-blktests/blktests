// SPDX-License-Identifier: GPL-3.0+
// Copyright (C) 2025 Keith Busch

/*
 * Simple test exercising the user metadata interfaces used by nvme passthrough
 * commands.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>

#ifndef _LINUX_NVME_IOCTL_H
#define _LINUX_NVME_IOCTL_H
struct nvme_passthru_cmd {
	__u8    opcode;
	__u8    flags;
	__u16   rsvd1;
	__u32   nsid;
	__u32   cdw2;
	__u32   cdw3;
	__u64   metadata;
	__u64   addr;
	__u32   metadata_len;
	__u32   data_len;
	__u32   cdw10;
	__u32   cdw11;
	__u32   cdw12;
	__u32   cdw13;
	__u32   cdw14;
	__u32   cdw15;
	__u32   timeout_ms;
	__u32   result;
};

#define NVME_IOCTL_ID		_IO('N', 0x40)
#define NVME_IOCTL_ADMIN_CMD    _IOWR('N', 0x41, struct nvme_passthru_cmd)
#define NVME_IOCTL_IO_CMD       _IOWR('N', 0x43, struct nvme_passthru_cmd)
#endif /* _UAPI_LINUX_NVME_IOCTL_H */

struct nvme_lbaf {
	__le16	ms;
	__u8	ds;
	__u8	rp;
};

struct nvme_id_ns {
	__le64	nsze;
	__le64	ncap;
	__le64	nuse;
	__u8	nsfeat;
	__u8	nlbaf;
	__u8	flbas;
	__u8	mc;
	__u8	dpc;
	__u8	dps;
	__u8	nmic;
	__u8	rescap;
	__u8	fpi;
	__u8	dlfeat;
	__le16	nawun;
	__le16	nawupf;
	__le16	nacwu;
	__le16	nabsn;
	__le16	nabo;
	__le16	nabspf;
	__le16	noiob;
	__u8	nvmcap[16];
	__le16	npwg;
	__le16	npwa;
	__le16	npdg;
	__le16	npda;
	__le16	nows;
	__u8	rsvd74[18];
	__le32	anagrpid;
	__u8	rsvd96[3];
	__u8	nsattr;
	__le16	nvmsetid;
	__le16	endgid;
	__u8	nguid[16];
	__u8	eui64[8];
	struct nvme_lbaf lbaf[64];
	__u8	vs[3712];
};

#define BUFFER_SIZE (32768)

int main(int argc, char **argv)
{
	int ret, fd, nsid, blocks, meta_buffer_size;
	void *buffer, *mptr = NULL, *meta = NULL;
	struct nvme_passthru_cmd cmd;
	struct nvme_lbaf lbaf;
	struct nvme_id_ns ns;

	__u64 block_size;
	__u16 meta_size;

	if (argc < 2) {
		fprintf(stderr, "usage: %s /dev/nvmeXnY", argv[0]);
		return EINVAL;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0)
		return fd;

	nsid = ioctl(fd, NVME_IOCTL_ID);
	if (nsid < 0) {
		perror("namespace id");
		return errno;
	}

	cmd = (struct nvme_passthru_cmd) {
		.opcode		= 0x6,
		.nsid		= nsid,
		.addr		= (__u64)(uintptr_t)&ns,
		.data_len       = sizeof(ns),
	};

	ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
	if (ret < 0) {
		perror("id-ns");
		return errno;
	}

	lbaf = ns.lbaf[ns.flbas & 0xf];
	block_size = 1 << lbaf.ds;
	meta_size = lbaf.ms;

	/* format not appropriate for this test */
	if (meta_size == 0) {
		fprintf(stderr, "Device format does not have metadata\n");
		return -EINVAL;
	}

	blocks = BUFFER_SIZE / block_size;
	meta_buffer_size = blocks * meta_size;

	buffer = malloc(BUFFER_SIZE);
	mptr = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
		MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (mptr == MAP_FAILED) {
		perror("mmap");
		return errno;
	}

	/* this should directly use the user space buffer */
	meta = mptr;
	cmd = (struct nvme_passthru_cmd) {
		.opcode		= 1,
		.nsid		= nsid,
		.addr		= (uintptr_t)buffer,
		.metadata       = (uintptr_t)meta,
		.data_len       = BUFFER_SIZE,
		.metadata_len   = meta_buffer_size,
		.cdw12		= blocks - 1,
	};

	ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	if (ret < 0) {
		perror("nvme-write");
		return ret;
	}

	cmd.opcode = 2;
	ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	if (ret < 0) {
		perror("nvme-read");
		return ret;
	}

	/*
	 * this offset should either force a kernel copy if we don't have
	 * contiguous pages, or test the device's metadata sgls
	 */
	meta = mptr + 4096 - 16;
	cmd.opcode = 1;
	cmd.metadata = (uintptr_t)meta;

	ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	if (ret < 0) {
		perror("nvme-write (offset)");
		return errno;
	}

	cmd.opcode = 2;
	ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	if (ret < 0) {
		perror("nvme-read (offset)");
		return errno;
	}

	/*
	 * This buffer is read-only, so should not be successful with commands
	 * where it is the destination (reads)
	 */
	mptr = mmap(NULL, 8192, PROT_READ, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (mptr == MAP_FAILED) {
		perror("mmap");
		return errno;
	}

	meta = mptr;

	cmd.opcode = 1;
	cmd.metadata = (uintptr_t)meta;
	ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	if (ret < 0) {
		perror("nvme-write (prot_read)");
		return ret;
	}

	cmd.opcode = 2;
	ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	if (ret == 0) {
		perror("nvme-read (expect Failure)");
		return EFAULT;
	}

	cmd.opcode = 1;
	cmd.metadata_len = 4096 * 20;
	ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
	if (ret == 0) {
			perror("nvme-write (large metadata_len)");
			return EFAULT;
	}

	return 0;
}
