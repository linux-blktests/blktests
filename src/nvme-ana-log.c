// SPDX-License-Identifier: GPL-3.0+
// Copyright (C) 2026 Jesse Taube

/*
 * Check that unused bytes in an NVMe ANA log page are zeroed.
 *
 * The ANA (Asymmetric Namespace Access) log page has a 16-byte header
 * followed by variable-length group descriptors. When the host requests
 * more bytes than the valid data occupies, the target must zero-fill the
 * remainder. A vulnerable target leaks stale kernel memory there.
 *
 * Strategy per attempt:
 *   1. Fill the host buffer with a easy-to-recognize pattern (0xAA).
 *   2. Request the ANA log page into that buffer.
 *   3. Parse the response header to find where valid data ends.
 *   4. Verify every byte beyond that point is zero.
 *   5. Repeat NR_ATTEMPTS times as the trailing data might incedentally be zeroed.
 *
 * Exit codes: 0 = all clean, 1 = stale data detected, 2 = setup error.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <endian.h>
#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BUF_LEN		(1024*256)
#define NR_ATTEMPTS	64


/* NUMD: Number of Dwords to return, 0's based
 * CDW10 for Get Log Page: NUMDL sits at bits [27:16]
 * (NVMe Base Spec, Figure 294)
 */
#define BYTES_TO_DWORDS(bytes)	((bytes) / sizeof(uint32_t))
#define NVME_LOG_NUMDL(bytes)	(((BYTES_TO_DWORDS(bytes) - 1) & 0xFFFF) << 16)

/* Broadcast namespace identifier */
#define NVME_NSID_ALL		0xFFFFFFFF

/* NVMe admin opcodes */
#define NVME_ADMIN_GET_LOG_PAGE	0x02


/* ANA log page identifier */
#define NVME_LOG_ANA		0x0C

/*
 * ANA structures mirrored from the kernel's include/linux/nvme.h.
 * Not exported to UAPI, so we define them here.
 */
struct nvme_ana_rsp_hdr {
	uint64_t	chgcnt;
	uint16_t	ngrps;
	uint16_t	rsvd10[3];
};

struct nvme_ana_group_desc {
	uint32_t	grpid;
	uint32_t	nnsids;
	uint64_t	chgcnt;
	uint8_t		state;
	uint8_t		rsvd17[15];
	uint32_t	nsids[];
};

static void hexdump(const char *label, const unsigned char *buf,
		    unsigned int len)
{
	unsigned int i, j;

	fprintf(stderr, "%s (%u bytes):\n", label, len);
	for (i = 0; i < len; i += 16) {
		fprintf(stderr, "0x%04x: ", i);
		for (j = 0; j < 16; j++) {
			if (i + j < len)
				fprintf(stderr, "%02x ", buf[i + j]);
			else
				fprintf(stderr, "   ");
			if (j == 7)
				fprintf(stderr, " ");
		}
		fprintf(stderr, " |");
		for (j = 0; j < 16 && i + j < len; j++)
			fprintf(stderr, "%c",
				isprint(buf[i + j]) ? buf[i + j] : '.');
		fprintf(stderr, "|\n");
	}
}

static int nvme_admin_cmd(int fd, struct nvme_passthru_cmd *cmd)
{
	int ret;

	ret = ioctl(fd, NVME_IOCTL_ADMIN_CMD, cmd);
	if (ret < 0)
		perror("NVME_IOCTL_ADMIN_CMD");
	else if (ret)
		fprintf(stderr, "NVMe admin status: %#x\n", ret);
	return ret;
}

/*
 * Request the ANA log page. NUMD (Number of Dwords, 0-based) is encoded
 * in CDW10[27:16] and tells the target how many bytes to return.
 */
static int get_ana_log(int fd, unsigned char *buf)
{
	struct nvme_passthru_cmd cmd = {
		.opcode		= NVME_ADMIN_GET_LOG_PAGE,
		.nsid		= NVME_NSID_ALL,
		.addr		= (uintptr_t)buf,
		.data_len	= BUF_LEN,
		.cdw10		= NVME_LOG_ANA | NVME_LOG_NUMDL(BUF_LEN),
	};

	return nvme_admin_cmd(fd, &cmd);
}

/*
 * Walk the ANA header and group descriptors to find the first byte past
 * the valid data. Everything from this offset to the end of the buffer
 * must be zero.
 */
static unsigned int ana_valid_end(const unsigned char *buf)
{
	const struct nvme_ana_rsp_hdr *hdr = (const void *)buf;
	unsigned int offset = sizeof(*hdr);
	uint16_t ngrps = le16toh(hdr->ngrps);
	uint16_t g;

	for (g = 0; g < ngrps && offset + sizeof(struct nvme_ana_group_desc) <= BUF_LEN; g++) {
		const struct nvme_ana_group_desc *desc = (const void *)(buf + offset);
		uint32_t nnsids = le32toh(desc->nnsids);

		offset += sizeof(*desc) + nnsids * sizeof(desc->nsids[0]);
	}

	return offset;
}

/* Return true if every byte in buf[start .. end) is zero. */
static bool is_zero(const unsigned char *buf,
		    unsigned int start, unsigned int end)
{
	unsigned int i;

	for (i = start; i < end; i++) {
		if (buf[i])
			return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	unsigned char *buf;
	void *p;
	int fd, ret, rc = 0;
	int i;

	if (argc != 2) {
		fprintf(stderr, "usage: %s /dev/nvmeX\n", argv[0]);
		return 2;
	}

	/*
	 * Page-align the buffer so that the kernel maps it for the transfer
	 * instead of bouncing it: a bounce buffer is zero-filled, which would
	 * hide the existing data and any bytes the target left unwritten.
	 */
	ret = posix_memalign(&p, 4096, BUF_LEN);
	if (ret) {
		fprintf(stderr, "posix_memalign: %s\n", strerror(ret));
		return 2;
	}
	buf = p;

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror(argv[1]);
		free(buf);
		return 2;
	}

	for (i = 0; i < NR_ATTEMPTS; i++) {
		unsigned int valid_end;
		bool clean;

		/* Set the buffer to a distinct pattern for debugging */
		memset(buf, 0xAA, BUF_LEN);

		if (get_ana_log(fd, buf)) {
			rc = 2;
			break;
		}

		valid_end = ana_valid_end(buf);
		clean = is_zero(buf, valid_end, BUF_LEN);

		if (!clean)
			hexdump("ANA log page", buf, BUF_LEN);

		fprintf(stderr, "attempt %d: valid data ends at offset 0x%02x, "
			"tail %s\n", i + 1, valid_end,
			clean ? "clean" : "stale");

		if (!clean) {
			rc = 1;
			break;
		}
	}

	close(fd);
	free(buf);
	return rc;
}
