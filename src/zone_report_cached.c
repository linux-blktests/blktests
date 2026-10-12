// SPDX-License-Identifier: GPL-3.0+
// Copyright (C) 2026 Shashank Mohan Jain
//
// zone_report_cached - exercise the BLKREPORTZONEV2 zone report ioctl.
//
// For a zoned block device it issues three reports and compares them:
//   R0 = BLKREPORTZONE                            (device report, legacy)
//   R1 = BLKREPORTZONEV2, flags = 0               (must equal R0)
//   R2 = BLKREPORTZONEV2, flags = BLK_ZONE_REP_CACHED (cached report)
//
// Usage: zone_report_cached <dev> <rw|ro>
//   rw: BLKOPENZONE zone 0, write one block to zone 1, BLKCLOSEZONE
//       zone 1 (so the device and the cached reports differ in the zone
//       conditions), then report.
//   ro: report only.
//
// Checks (program exits non-zero if any fails; details go to stdout,
// which the test redirects to $FULL):
//   A : R1 == R0 for every zone (start/len/capacity/cond).  Fails when
//       BLKREPORTZONEV2 with flags == 0 returns the cached report
//       instead of a device report.
//   B : the last zone, which may be smaller than the zone size, has the
//       same len in the cached report R2 as in the device report R0, and
//       R2.last.start + R2.last.len does not exceed the device capacity.
// An informational line reports whether the cached branch was taken
// (the cached report carries BLK_ZONE_COND_ACTIVE where the device
// report does not); this is advisory and does not affect the exit code.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#ifdef HAVE_LINUX_BLKZONED_H
#include <linux/blkzoned.h>
#endif
#include <linux/fs.h>

#ifndef BLKREPORTZONEV2
#define BLKREPORTZONEV2 _IOWR(0x12, 142, struct blk_zone_report)
#endif
#ifndef BLK_ZONE_REP_CACHED
#define BLK_ZONE_REP_CACHED (1U << 31)
#endif
#ifndef BLK_ZONE_COND_ACTIVE
#define BLK_ZONE_COND_ACTIVE 0xFF
#endif

static const char *condname(unsigned int c)
{
	switch (c) {
	case 0x0: return "NOT_WP";
	case 0x1: return "EMPTY";
	case 0x2: return "IMP_OPEN";
	case 0x3: return "EXP_OPEN";
	case 0x4: return "CLOSED";
	case 0xd: return "READONLY";
	case 0xe: return "FULL";
	case 0xf: return "OFFLINE";
	case 0xff: return "ACTIVE";
	default: return "?";
	}
}

static struct blk_zone_report *do_report(int fd, unsigned int nr,
					 unsigned long cmd, unsigned int flags,
					 const char *tag)
{
	size_t sz = sizeof(struct blk_zone_report) +
		    nr * sizeof(struct blk_zone);
	struct blk_zone_report *r = calloc(1, sz);

	if (!r)
		return NULL;
	r->sector = 0;
	r->nr_zones = nr;
	r->flags = flags;
	if (ioctl(fd, cmd, r) < 0) {
		fprintf(stderr, "ioctl %s failed: %s\n", tag, strerror(errno));
		free(r);
		return NULL;
	}
	return r;
}

static void print_report(const char *tag, struct blk_zone_report *r)
{
	unsigned int i;

	printf("  %-22s nr=%u flags=0x%x\n", tag, r->nr_zones, r->flags);
	for (i = 0; i < r->nr_zones; i++) {
		struct blk_zone *z = &r->zones[i];

		printf("    zone %u: start=%llu len=%llu cap=%llu wp=%llu type=%u cond=0x%x(%s)\n",
		       i, (unsigned long long)z->start,
		       (unsigned long long)z->len,
		       (unsigned long long)z->capacity,
		       (unsigned long long)z->wp, z->type, z->cond,
		       condname(z->cond));
	}
}

int main(int argc, char **argv)
{
	const char *dev;
	int do_rw, fail = 0, fd;
	__u32 zsz = 0, nrz = 0;
	uint64_t cap_bytes = 0, cap_sectors;
	struct blk_zone_report *R0, *R1, *R2;
	struct blk_zone *l0, *l2;
	unsigned int i, last;
	int a_ok, b_ok, cached_active = 0, flags0_active = 0, smaller;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <dev> <rw|ro>\n", argv[0]);
		return 2;
	}
	dev = argv[1];
	do_rw = strcmp(argv[2], "rw") == 0;

	fd = open(dev, O_RDWR | O_DIRECT);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 2;
	}

	ioctl(fd, BLKGETZONESZ, &zsz);
	ioctl(fd, BLKGETNRZONES, &nrz);
	ioctl(fd, BLKGETSIZE64, &cap_bytes);
	cap_sectors = cap_bytes / 512;
	printf("== %s ==\n", dev);
	printf("  zone_size_sectors=%u nr_zones=%u capacity_sectors=%llu\n",
	       zsz, nrz, (unsigned long long)cap_sectors);

	if (do_rw) {
		struct blk_zone_range zr;
		void *buf = NULL;

		/* open zone 0 */
		zr.sector = 0;
		zr.nr_sectors = zsz;
		if (ioctl(fd, BLKOPENZONE, &zr) < 0)
			fprintf(stderr, "  BLKOPENZONE z0: %s\n",
				strerror(errno));
		/* write one 4096-byte block at the start of zone 1 */
		if (posix_memalign(&buf, 4096, 4096) == 0) {
			off_t off = (off_t)zsz * 512;
			ssize_t w;

			memset(buf, 0xab, 4096);
			w = pwrite(fd, buf, 4096, off);
			if (w != 4096)
				fprintf(stderr, "  write z1: %s (ret %zd)\n",
					strerror(errno), w);
			free(buf);
		}
		/* close zone 1 */
		zr.sector = zsz;
		zr.nr_sectors = zsz;
		if (ioctl(fd, BLKCLOSEZONE, &zr) < 0)
			fprintf(stderr, "  BLKCLOSEZONE z1: %s\n",
				strerror(errno));
		fsync(fd);
	}

	R0 = do_report(fd, nrz, BLKREPORTZONE, 0, "BLKREPORTZONE");
	R1 = do_report(fd, nrz, BLKREPORTZONEV2, 0, "BLKREPORTZONEV2 flags=0");
	R2 = do_report(fd, nrz, BLKREPORTZONEV2, BLK_ZONE_REP_CACHED,
		       "BLKREPORTZONEV2 CACHED");
	if (!R0 || !R1 || !R2) {
		free(R0);
		free(R1);
		free(R2);
		close(fd);
		return 2;
	}
	print_report("BLKREPORTZONE", R0);
	print_report("REPV2 flags=0", R1);
	print_report("REPV2 CACHED", R2);

	/* check A: R1 (device branch) must equal R0 for every zone */
	a_ok = (R0->nr_zones == R1->nr_zones);
	for (i = 0; a_ok && i < R0->nr_zones; i++) {
		struct blk_zone *a = &R0->zones[i], *b = &R1->zones[i];

		if (a->start != b->start || a->len != b->len ||
		    a->capacity != b->capacity || a->cond != b->cond)
			a_ok = 0;
	}
	printf("CHECK A  (REPV2 flags=0 == BLKREPORTZONE device report): %s\n",
	       a_ok ? "PASS" : "FAIL");
	if (!a_ok)
		fail = 1;

	/* informational: was the cached branch taken when the flag was set? */
	for (i = 0; i < R2->nr_zones; i++)
		if (R2->zones[i].cond == BLK_ZONE_COND_ACTIVE)
			cached_active = 1;
	for (i = 0; i < R1->nr_zones; i++)
		if (R1->zones[i].cond == BLK_ZONE_COND_ACTIVE)
			flags0_active = 1;
	if (cached_active && !flags0_active)
		printf("INFO  CACHED-BR: cached report shows ACTIVE, flags=0 does not (cached branch taken only for the flag)\n");
	else if (!cached_active)
		printf("INFO  CACHED-BR: device has no cached report here; nothing to distinguish\n");
	else
		printf("INFO  CACHED-BR: flags=0 unexpectedly shows ACTIVE\n");

	/* check B: smaller last zone length correct in the cached report */
	last = R0->nr_zones - 1;
	l0 = &R0->zones[last];
	l2 = &R2->zones[last];
	smaller = (l0->len < zsz);
	b_ok = (l2->len == l0->len) && (l2->start + l2->len == cap_sectors);
	printf("CHECK B  (last zone len in CACHED == device, within capacity): %s  [last zone %s: dev len=%llu cached len=%llu start+len=%llu cap=%llu]\n",
	       b_ok ? "PASS" : "FAIL",
	       smaller ? "IS SMALLER than zone size" : "is full size",
	       (unsigned long long)l0->len, (unsigned long long)l2->len,
	       (unsigned long long)(l2->start + l2->len),
	       (unsigned long long)cap_sectors);
	if (!b_ok)
		fail = 1;

	free(R0);
	free(R1);
	free(R2);
	close(fd);
	printf("RESULT %s: %s\n", dev, fail ? "FAIL" : "OK");
	return fail ? 1 : 0;
}
