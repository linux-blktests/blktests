// SPDX-License-Identifier: GPL-3.0+
// Copyright (C) 2023 Ming Lei

/*
 * io_uring based mini ublk implementation with null/loop target,
 * for test purpose only.
 *
 * So please keep it clean & simple & reliable.
 */

#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <getopt.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <liburing.h>
#include <linux/ublk_cmd.h>

#define CTRL_DEV		"/dev/ublk-control"
#define UBLKC_DEV		"/dev/ublkc"
#define UBLK_CTRL_RING_DEPTH            32

/* queue idle timeout */
#define UBLKSRV_IO_IDLE_SECS		20

#define UBLK_IO_MAX_BYTES               65536
#define UBLK_MAX_QUEUES                 4
#define UBLK_QUEUE_DEPTH                128

#define UBLK_DBG_DEV            (1U << 0)
#define UBLK_DBG_QUEUE          (1U << 1)
#define UBLK_DBG_IO_CMD         (1U << 2)
#define UBLK_DBG_IO             (1U << 3)
#define UBLK_DBG_CTRL_CMD       (1U << 4)
#define UBLK_LOG		(1U << 5)

struct ublk_dev;
struct ublk_queue;

struct ublk_ctrl_cmd_data {
	__u32 cmd_op;
#define CTRL_CMD_HAS_DATA	1
#define CTRL_CMD_HAS_BUF	2
	__u32 flags;

	__u64 data[2];
	__u64 addr;
	__u32 len;
};

struct ublk_io {
	char *buf_addr;

#define UBLKSRV_NEED_FETCH_RQ		(1UL << 0)
#define UBLKSRV_NEED_COMMIT_RQ_COMP	(1UL << 1)
#define UBLKSRV_IO_FREE			(1UL << 2)
	unsigned int flags;

	unsigned int result;
};

struct ublk_tgt_ops {
	const char *name;
	int (*init_tgt)(struct ublk_dev *);
	void (*deinit_tgt)(struct ublk_dev *);

	int (*queue_io)(struct ublk_queue *, int tag);
	void (*tgt_io_done)(struct ublk_queue *,
			int tag, const struct io_uring_cqe *);
	int (*recover_tgt)(struct ublk_dev *);
};

struct ublk_tgt {
	unsigned long dev_size;
	const struct ublk_tgt_ops *ops;
	int argc;
	char **argv;
	struct ublk_params params;
};

struct ublk_queue {
	int q_id;
	int q_depth;
	unsigned int cmd_inflight;
	unsigned int io_inflight;
	struct ublk_dev *dev;
	const struct ublk_tgt_ops *tgt_ops;
	char *io_cmd_buf;
	struct io_uring ring;
	struct ublk_io ios[UBLK_QUEUE_DEPTH];
#define UBLKSRV_QUEUE_STOPPING	(1U << 0)
#define UBLKSRV_QUEUE_IDLE	(1U << 1)
	unsigned state;
	pid_t tid;
	pthread_t thread;
};

struct ublk_dev {
	struct ublk_tgt tgt;
	struct ublksrv_ctrl_dev_info  dev_info;
	struct ublk_queue q[UBLK_MAX_QUEUES];

	int fds[2];	/* fds[0] points to /dev/ublkcN */
	int nr_fds;
	int ctrl_fd;
	struct io_uring ring;
};

#ifndef offsetof
#define offsetof(TYPE, MEMBER)  ((size_t)&((TYPE *)0)->MEMBER)
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({                              \
	unsigned long __mptr = (unsigned long)(ptr);                    \
	((type *)(__mptr - offsetof(type, member))); })
#endif

#define round_up(val, rnd) \
	(((val) + ((rnd) - 1)) & ~((rnd) - 1))

#define ublk_assert(x)  do { \
	if (!(x)) {     \
		ublk_err("%s %d: assert!\n", __func__, __LINE__); \
		assert(x);      \
	}       \
} while (0)

static const struct ublk_tgt_ops *ublk_find_tgt(const char *name);

static unsigned int ublk_dbg_mask = UBLK_LOG;

static inline unsigned ilog2(unsigned x)
{
    return sizeof(unsigned) * 8 - 1 - __builtin_clz(x);
}

static inline int is_target_io(__u64 user_data)
{
	return (user_data & (1ULL << 63)) != 0;
}

static inline __u64 build_user_data(unsigned tag, unsigned op,
		unsigned tgt_data, unsigned is_target_io)
{
	assert(!(tag >> 16) && !(op >> 8) && !(tgt_data >> 16));

	return tag | (op << 16) | (tgt_data << 24) | (__u64)is_target_io << 63;
}

static inline unsigned int user_data_to_tag(__u64 user_data)
{
	return user_data & 0xffff;
}

static inline unsigned int user_data_to_op(__u64 user_data)
{
	return (user_data >> 16) & 0xff;
}

static void ublk_err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
}

static void ublk_log(const char *fmt, ...)
{
	if (ublk_dbg_mask & UBLK_LOG) {
		va_list ap;

		va_start(ap, fmt);
		vfprintf(stdout, fmt, ap);
	}
}

static void ublk_dbg(int level, const char *fmt, ...)
{
	if (level & ublk_dbg_mask) {
		va_list ap;
		va_start(ap, fmt);
		vfprintf(stdout, fmt, ap);
        }
}

static inline void *ublk_get_sqe_cmd(const struct io_uring_sqe *sqe)
{
	return (void *)&sqe->addr3;
}

static inline void ublk_mark_io_done(struct ublk_io *io, int res)
{
	io->flags |= (UBLKSRV_NEED_COMMIT_RQ_COMP | UBLKSRV_IO_FREE);
	io->result = res;
}

static inline const struct ublksrv_io_desc *ublk_get_iod(
                const struct ublk_queue *q, int tag)
{
        return (struct ublksrv_io_desc *)
                &(q->io_cmd_buf[tag * sizeof(struct ublksrv_io_desc)]);
}

static inline void ublk_set_sqe_cmd_op(struct io_uring_sqe *sqe,
		__u32 cmd_op)
{
        __u32 *addr = (__u32 *)&sqe->off;

        addr[0] = cmd_op;
        addr[1] = 0;
}

static inline int ublk_setup_ring(struct io_uring *r, int depth,
		int cq_depth, unsigned flags)
{
	struct io_uring_params p;

	memset(&p, 0, sizeof(p));
	p.flags = flags | IORING_SETUP_CQSIZE;
	p.cq_entries = cq_depth;

	return io_uring_queue_init_params(depth, r, &p);
}

static inline void ublk_ctrl_init_cmd(struct ublk_dev *dev,
		struct io_uring_sqe *sqe,
		struct ublk_ctrl_cmd_data *data)
{
	struct ublksrv_ctrl_dev_info *info = &dev->dev_info;
	struct ublksrv_ctrl_cmd *cmd = (struct ublksrv_ctrl_cmd *)ublk_get_sqe_cmd(sqe);

	sqe->fd = dev->ctrl_fd;
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->ioprio = 0;

	if (data->flags & CTRL_CMD_HAS_BUF) {
		cmd->addr = data->addr;
		cmd->len = data->len;
	}

	if (data->flags & CTRL_CMD_HAS_DATA)
		cmd->data[0] = data->data[0];

	cmd->dev_id = info->dev_id;
	cmd->queue_id = -1;

	ublk_set_sqe_cmd_op(sqe, data->cmd_op);

	io_uring_sqe_set_data(sqe, cmd);
}

static int __ublk_ctrl_cmd(struct ublk_dev *dev,
		struct ublk_ctrl_cmd_data *data)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int ret = -EINVAL;

	sqe = io_uring_get_sqe(&dev->ring);
	if (!sqe) {
		ublk_err("%s: can't get sqe ret %d\n", __func__, ret);
		return ret;
	}

	ublk_ctrl_init_cmd(dev, sqe, data);

	ret = io_uring_submit(&dev->ring);
	if (ret < 0) {
		ublk_err("uring submit ret %d\n", ret);
		return ret;
	}

	ret = io_uring_wait_cqe(&dev->ring, &cqe);
	if (ret < 0) {
		ublk_err("wait cqe: %s\n", strerror(-ret));
		return ret;
	}
	io_uring_cqe_seen(&dev->ring, cqe);

	return cqe->res;
}

int ublk_ctrl_stop_dev(struct ublk_dev *dev)
{
	struct ublk_ctrl_cmd_data data = {
		.cmd_op	= UBLK_U_CMD_STOP_DEV,
	};

	return __ublk_ctrl_cmd(dev, &data);
}

int ublk_ctrl_start_dev(struct ublk_dev *dev,
		int daemon_pid)
{
	struct ublk_ctrl_cmd_data data = {
		.cmd_op	= UBLK_U_CMD_START_DEV,
		.flags	= CTRL_CMD_HAS_DATA,
	};

	dev->dev_info.ublksrv_pid = data.data[0] = daemon_pid;

	return __ublk_ctrl_cmd(dev, &data);
}

int ublk_ctrl_add_dev(struct ublk_dev *dev)
{
	struct ublk_ctrl_cmd_data data = {
		.cmd_op	= UBLK_U_CMD_ADD_DEV,
		.flags	= CTRL_CMD_HAS_BUF,
		.addr = (__u64)&dev->dev_info,
		.len = sizeof(struct ublksrv_ctrl_dev_info),
	};

	return __ublk_ctrl_cmd(dev, &data);
}

int ublk_ctrl_del_dev(struct ublk_dev *dev)
{
	struct ublk_ctrl_cmd_data data = {
		.cmd_op = UBLK_U_CMD_DEL_DEV,
		.flags = 0,
	};

	return __ublk_ctrl_cmd(dev, &data);
}

int ublk_ctrl_get_info(struct ublk_dev *dev)
{
	struct ublk_ctrl_cmd_data data = {
		.cmd_op	= UBLK_U_CMD_GET_DEV_INFO,
		.flags	= CTRL_CMD_HAS_BUF,
		.addr = (__u64)&dev->dev_info,
		.len = sizeof(struct ublksrv_ctrl_dev_info),
	};

	return __ublk_ctrl_cmd(dev, &data);
}

int ublk_ctrl_set_params(struct ublk_dev *dev,
		struct ublk_params *params)
{
	struct ublk_ctrl_cmd_data data = {
		.cmd_op	= UBLK_U_CMD_SET_PARAMS,
		.flags	= CTRL_CMD_HAS_BUF,
		.addr = (__u64)params,
		.len = sizeof(*params),
	};
	params->len = sizeof(*params);
	return __ublk_ctrl_cmd(dev, &data);
}

static int ublk_ctrl_get_params(struct ublk_dev *dev,
		struct ublk_params *params)
{
	struct ublk_ctrl_cmd_data data = {
		.cmd_op	= UBLK_U_CMD_GET_PARAMS,
		.flags	= CTRL_CMD_HAS_BUF,
		.addr = (__u64)params,
		.len = sizeof(*params),
	};

	params->len = sizeof(*params);

	return __ublk_ctrl_cmd(dev, &data);
}

static int ublk_ctrl_start_user_recover(struct ublk_dev *dev)
{
	struct ublk_ctrl_cmd_data data = {
		.cmd_op	= UBLK_U_CMD_START_USER_RECOVERY,
		.flags	= 0,
	};

	return __ublk_ctrl_cmd(dev, &data);
}

static int ublk_ctrl_end_user_recover(struct ublk_dev *dev,
		int daemon_pid)
{
	struct ublk_ctrl_cmd_data data = {
		.cmd_op	= UBLK_U_CMD_END_USER_RECOVERY,
		.flags	= CTRL_CMD_HAS_DATA,
	};

	dev->dev_info.ublksrv_pid = data.data[0] = daemon_pid;

	return __ublk_ctrl_cmd(dev, &data);
}

static const char *ublk_dev_state_desc(struct ublk_dev *dev)
{
	switch (dev->dev_info.state) {
	case UBLK_S_DEV_DEAD:
		return "DEAD";
	case UBLK_S_DEV_LIVE:
		return "LIVE";
	case UBLK_S_DEV_QUIESCED:
		return "QUIESCED";
	default:
		return "UNKNOWN";
	};
}

static void ublk_ctrl_dump(struct ublk_dev *dev, bool show_queue)
{
	struct ublksrv_ctrl_dev_info *info = &dev->dev_info;
	int ret;
	struct ublk_params p;

	ret = ublk_ctrl_get_params(dev, &p);
	if (ret < 0) {
		ublk_err("failed to get params %m\n");
		return;
	}

	ublk_log("dev id %d: nr_hw_queues %d queue_depth %d block size %d dev_capacity %lld\n",
			info->dev_id,
                        info->nr_hw_queues, info->queue_depth,
                        1 << p.basic.logical_bs_shift, p.basic.dev_sectors);
	ublk_log("\tmax rq size %d daemon pid %d flags 0x%llx state %s\n",
                        info->max_io_buf_bytes,
			info->ublksrv_pid, info->flags,
			ublk_dev_state_desc(dev));
	if (show_queue) {
		int i;

		for (i = 0; i < dev->dev_info.nr_hw_queues; i++)
			ublk_log("\tqueue 0 tid: %d\n", dev->q[i].tid);
	}
	fflush(stdout);
}

static void ublk_ctrl_deinit(struct ublk_dev *dev)
{
	close(dev->ctrl_fd);
	free(dev);
}

static struct ublk_dev *ublk_ctrl_init()
{
	struct ublk_dev *dev = (struct ublk_dev *)calloc(1, sizeof(*dev));
	struct ublksrv_ctrl_dev_info *info = &dev->dev_info;
	int ret;

	dev->ctrl_fd = open(CTRL_DEV, O_RDWR);
	if (dev->ctrl_fd < 0) {
		ublk_err("control dev %s can't be opened: %m %d\n", CTRL_DEV, errno);
		exit(dev->ctrl_fd);
	}
	info->max_io_buf_bytes = UBLK_IO_MAX_BYTES;

	ret = ublk_setup_ring(&dev->ring, UBLK_CTRL_RING_DEPTH,
			UBLK_CTRL_RING_DEPTH, IORING_SETUP_SQE128);
	if (ret < 0) {
		ublk_err("queue_init: %s\n", strerror(-ret));
		free(dev);
		return NULL;
	}
	dev->nr_fds = 1;

	return dev;
}

static int __ublk_queue_cmd_buf_sz(unsigned depth)
{
	int size =  depth * sizeof(struct ublksrv_io_desc);
	unsigned int page_sz = getpagesize();

	return round_up(size, page_sz);
}

static int ublk_queue_max_cmd_buf_sz(void)
{
	return __ublk_queue_cmd_buf_sz(UBLK_MAX_QUEUE_DEPTH);
}

static int ublk_queue_cmd_buf_sz(struct ublk_queue *q)
{
	return __ublk_queue_cmd_buf_sz(q->q_depth);
}

static void ublk_queue_deinit(struct ublk_queue *q)
{
	int i;
	int nr_ios = q->q_depth;

	io_uring_unregister_ring_fd(&q->ring);

	if (q->ring.ring_fd > 0) {
		io_uring_unregister_files(&q->ring);
		close(q->ring.ring_fd);
		q->ring.ring_fd = -1;
	}

	if (q->io_cmd_buf)
		munmap(q->io_cmd_buf, ublk_queue_cmd_buf_sz(q));

	for (i = 0; i < nr_ios; i++)
		free(q->ios[i].buf_addr);
}

static int ublk_queue_init(struct ublk_queue *q)
{
	struct ublk_dev *dev = q->dev;
	int depth = dev->dev_info.queue_depth;
	int i, ret = -1;
	int cmd_buf_size, io_buf_size;
	unsigned long off;
	int ring_depth = depth, cq_depth = depth;

	q->tgt_ops = dev->tgt.ops;
	q->state = 0;
	q->q_depth = depth;
	q->cmd_inflight = 0;
	q->tid = gettid();

	cmd_buf_size = ublk_queue_cmd_buf_sz(q);
	off = UBLKSRV_CMD_BUF_OFFSET + q->q_id * ublk_queue_max_cmd_buf_sz();
	q->io_cmd_buf = (char *)mmap(0, cmd_buf_size, PROT_READ,
			MAP_SHARED | MAP_POPULATE, dev->fds[0], off);
	if (q->io_cmd_buf == MAP_FAILED) {
		ublk_err("ublk dev %d queue %d map io_cmd_buf failed %m\n",
				q->dev->dev_info.dev_id, q->q_id);
		goto fail;
	}

	io_buf_size = dev->dev_info.max_io_buf_bytes;
	for (i = 0; i < q->q_depth; i++) {
		q->ios[i].buf_addr = NULL;

		if (posix_memalign((void **)&q->ios[i].buf_addr,
					getpagesize(), io_buf_size)) {
			ublk_err("ublk dev %d queue %d io %d posix_memalign failed %m\n",
					dev->dev_info.dev_id, q->q_id, i);
			goto fail;
		}
		q->ios[i].flags = UBLKSRV_NEED_FETCH_RQ | UBLKSRV_IO_FREE;
	}

	ret = ublk_setup_ring(&q->ring, ring_depth, cq_depth,
			IORING_SETUP_SQE128 | IORING_SETUP_COOP_TASKRUN);
	if (ret < 0) {
		ublk_err("ublk dev %d queue %d setup io_uring failed %d\n",
				q->dev->dev_info.dev_id, q->q_id, ret);
		goto fail;
	}

	io_uring_register_ring_fd(&q->ring);

	ret = io_uring_register_files(&q->ring, dev->fds, dev->nr_fds);
	if (ret) {
		ublk_err("ublk dev %d queue %d register files failed %d\n",
				q->dev->dev_info.dev_id, q->q_id, ret);
		goto fail;
	}

	return 0;
 fail:
	ublk_queue_deinit(q);
	ublk_err("ublk dev %d queue %d failed\n",
			dev->dev_info.dev_id, q->q_id);
	return -ENOMEM;
}

static int ublk_dev_prep(struct ublk_dev *dev)
{
	int dev_id = dev->dev_info.dev_id;
	char buf[64];
	int ret = 0;

	snprintf(buf, 64, "%s%d", UBLKC_DEV, dev_id);
	dev->fds[0] = open(buf, O_RDWR);
	if (dev->fds[0] < 0) {
		ret = -EBADF;
		ublk_err("can't open %s, ret %d\n", buf, dev->fds[0]);
		goto fail;
	}

	if (dev->dev_info.state != UBLK_S_DEV_QUIESCED && dev->tgt.ops->init_tgt)
		ret = dev->tgt.ops->init_tgt(dev);

	else if (dev->dev_info.state == UBLK_S_DEV_QUIESCED && dev->tgt.ops->recover_tgt)
		ret = dev->tgt.ops->recover_tgt(dev);

	return ret;
fail:
	close(dev->fds[0]);
	return ret;
}

static void ublk_dev_unprep(struct ublk_dev *dev)
{
	if (dev->tgt.ops->deinit_tgt)
		dev->tgt.ops->deinit_tgt(dev);
	close(dev->fds[0]);
}

static int ublk_queue_io_cmd(struct ublk_queue *q,
		struct ublk_io *io, unsigned tag)
{
	struct ublksrv_io_cmd *cmd;
	struct io_uring_sqe *sqe;
	unsigned int cmd_op = 0;
	__u64 user_data;

	/* only freed io can be issued */
	if (!(io->flags & UBLKSRV_IO_FREE))
		return 0;

	/* we issue because we need either fetching or committing */
	if (!(io->flags &
		(UBLKSRV_NEED_FETCH_RQ | UBLKSRV_NEED_COMMIT_RQ_COMP)))
		return 0;

	if (io->flags & UBLKSRV_NEED_COMMIT_RQ_COMP)
		cmd_op = UBLK_U_IO_COMMIT_AND_FETCH_REQ;
	else if (io->flags & UBLKSRV_NEED_FETCH_RQ)
		cmd_op = UBLK_U_IO_FETCH_REQ;

	sqe = io_uring_get_sqe(&q->ring);
	if (!sqe) {
		ublk_err("%s: run out of sqe %d, tag %d\n",
				__func__, q->q_id, tag);
		return -1;
	}

	cmd = (struct ublksrv_io_cmd *)ublk_get_sqe_cmd(sqe);

	if (io->flags & UBLKSRV_NEED_COMMIT_RQ_COMP)
		cmd->result = io->result;

	/* These fields should be written once, never change */
	ublk_set_sqe_cmd_op(sqe, cmd_op);
	sqe->fd		= 0;	/* dev->fds[0] */
	sqe->opcode	= IORING_OP_URING_CMD;
	sqe->flags	= IOSQE_FIXED_FILE;
	sqe->rw_flags	= 0;
	cmd->tag	= tag;
	cmd->addr	= (__u64)io->buf_addr;
	cmd->q_id	= q->q_id;

	user_data = build_user_data(tag, _IOC_NR(cmd_op), 0, 0);
	io_uring_sqe_set_data64(sqe, user_data);

	io->flags = 0;

	q->cmd_inflight += 1;

	ublk_dbg(UBLK_DBG_IO_CMD, "%s: (qid %d tag %u cmd_op %u) iof %x stopping %d\n",
			__func__, q->q_id, tag, _IOC_NR(cmd_op),
			io->flags, !!(q->state & UBLKSRV_QUEUE_STOPPING));
	return 1;
}

static int ublk_complete_io(struct ublk_queue *q,
		unsigned tag, int res)
{
	struct ublk_io *io = &q->ios[tag];

	ublk_mark_io_done(io, res);

	return ublk_queue_io_cmd(q, io, tag);
}

static void ublk_submit_fetch_commands(struct ublk_queue *q)
{
	int i = 0;

	for (i = 0; i < q->q_depth; i++)
		ublk_queue_io_cmd(q, &q->ios[i], i);
}

static int ublk_queue_is_idle(struct ublk_queue *q)
{
	return !io_uring_sq_ready(&q->ring) && !q->io_inflight;
}

static int ublk_queue_is_done(struct ublk_queue *q)
{
	return (q->state & UBLKSRV_QUEUE_STOPPING) && ublk_queue_is_idle(q);
}

static void ublk_queue_discard_io_pages(struct ublk_queue *q)
{
	const struct ublk_dev *dev = q->dev;
	unsigned int io_buf_size = dev->dev_info.max_io_buf_bytes;
	int i = 0;

	for (i = 0; i < q->q_depth; i++)
		madvise(q->ios[i].buf_addr, io_buf_size, MADV_DONTNEED);
}

static void ublk_queue_idle_enter(struct ublk_queue *q)
{
	if (q->state & UBLKSRV_QUEUE_IDLE)
		return;

	ublk_dbg(UBLK_DBG_QUEUE, "dev%d-q%d: enter idle %x\n",
			q->dev->dev_info.dev_id, q->q_id, q->state);
	ublk_queue_discard_io_pages(q);
	q->state |= UBLKSRV_QUEUE_IDLE;
}

static void ublk_queue_idle_exit(struct ublk_queue *q)
{
	if (q->state & UBLKSRV_QUEUE_IDLE) {
		ublk_dbg(UBLK_DBG_QUEUE, "dev%d-q%d: exit idle %x\n",
			q->dev->dev_info.dev_id, q->q_id, q->state);
		q->state &= ~UBLKSRV_QUEUE_IDLE;
	}
}

static inline void ublksrv_handle_tgt_cqe(struct ublk_queue *q,
		struct io_uring_cqe *cqe)
{
	unsigned tag = user_data_to_tag(cqe->user_data);

	if (cqe->res < 0 && cqe->res != -EAGAIN)
		ublk_err("%s: failed tgt io: res %d qid %u tag %u, cmd_op %u\n",
			__func__, cqe->res, q->q_id,
			user_data_to_tag(cqe->user_data),
			user_data_to_op(cqe->user_data));

	if (q->tgt_ops->tgt_io_done)
		q->tgt_ops->tgt_io_done(q, tag, cqe);
}

static void ublk_handle_cqe(struct io_uring *r,
		struct io_uring_cqe *cqe, void *data)
{
	struct ublk_queue *q = container_of(r, struct ublk_queue, ring);
	unsigned tag = user_data_to_tag(cqe->user_data);
	unsigned cmd_op = user_data_to_op(cqe->user_data);
	int fetch = (cqe->res != UBLK_IO_RES_ABORT) &&
		!(q->state & UBLKSRV_QUEUE_STOPPING);
	struct ublk_io *io;

	ublk_dbg(UBLK_DBG_IO_CMD, "%s: res %d (qid %d tag %u cmd_op %u target %d) stopping %d\n",
			__func__, cqe->res, q->q_id, tag, cmd_op,
			is_target_io(cqe->user_data),
			(q->state & UBLKSRV_QUEUE_STOPPING));

	/* Don't retrieve io in case of target io */
	if (is_target_io(cqe->user_data)) {
		ublksrv_handle_tgt_cqe(q, cqe);
		return;
	}

	io = &q->ios[tag];
	q->cmd_inflight--;

	if (!fetch) {
		q->state |= UBLKSRV_QUEUE_STOPPING;
		io->flags &= ~UBLKSRV_NEED_FETCH_RQ;
	}

	if (cqe->res == UBLK_IO_RES_OK) {
		ublk_assert(tag < q->q_depth);
		q->tgt_ops->queue_io(q, tag);
	} else {
		/*
		 * COMMIT_REQ will be completed immediately since no fetching
		 * piggyback is required.
		 *
		 * Marking IO_FREE only, then this io won't be issued since
		 * we only issue io with (UBLKSRV_IO_FREE | UBLKSRV_NEED_*)
		 *
		 * */
		io->flags = UBLKSRV_IO_FREE;
	}
}

static int ublk_reap_events_uring(struct io_uring *r)
{
	struct io_uring_cqe *cqe;
	unsigned head;
	int count = 0;

	io_uring_for_each_cqe(r, head, cqe) {
		ublk_handle_cqe(r, cqe, NULL);
		count += 1;
	}
	io_uring_cq_advance(r, count);

	return count;
}

static int ublk_process_io(struct ublk_queue *q)
{
	int ret, reapped;
	struct __kernel_timespec ts = {
		.tv_sec = UBLKSRV_IO_IDLE_SECS,
		.tv_nsec = 0
        };
	struct __kernel_timespec *tsp = (q->state & UBLKSRV_QUEUE_IDLE) ?
		NULL : &ts;
	struct io_uring_cqe *cqe;

	ublk_dbg(UBLK_DBG_QUEUE, "dev%d-q%d: to_submit %d inflight cmd %u stopping %d\n",
				q->dev->dev_info.dev_id,
				q->q_id, io_uring_sq_ready(&q->ring),
				q->cmd_inflight,
				(q->state & UBLKSRV_QUEUE_STOPPING));

	if (ublk_queue_is_done(q))
		return -ENODEV;

	ret = io_uring_submit_and_wait_timeout(&q->ring, &cqe, 1, tsp, NULL);
	reapped = ublk_reap_events_uring(&q->ring);

	ublk_dbg(UBLK_DBG_QUEUE, "submit result %d, reapped %d stop %d idle %d\n",
			ret, reapped, (q->state & UBLKSRV_QUEUE_STOPPING),
			(q->state & UBLKSRV_QUEUE_IDLE));

	if (!(q->state & UBLKSRV_QUEUE_STOPPING)) {
		if (ret == -ETIME && reapped == 0 && ublk_queue_is_idle(q))
			ublk_queue_idle_enter(q);
		else
			ublk_queue_idle_exit(q);
	}
	return reapped;
}

static void *ublk_io_handler_fn(void *data)
{
	struct ublk_queue *q = data;
	int dev_id = q->dev->dev_info.dev_id;
	int ret;

	ret = ublk_queue_init(q);
	if (ret) {
		ublk_err("ublk dev %d queue %d init queue failed\n",
				dev_id, q->q_id);
		return NULL;
	}

	/* submit all io commands to ublk driver */
	ublk_submit_fetch_commands(q);

	ublk_dbg(UBLK_DBG_QUEUE, "tid %d: ublk dev %d queue %d started\n",
			gettid(),
			dev_id, q->q_id);
	do {
		if (ublk_process_io(q) < 0)
			break;
	} while (1);

	ublk_dbg(UBLK_DBG_QUEUE, "ublk dev %d queue %d exited\n", dev_id, q->q_id);
	ublk_queue_deinit(q);
	return NULL;
}

static void ublk_set_parameters(struct ublk_dev *dev)
{
	int ret;

	ret = ublk_ctrl_set_params(dev, &dev->tgt.params);
	if (ret)
		ublk_err("dev %d set basic parameter failed %d\n",
				dev->dev_info.dev_id, ret);
}

static int ublk_start_daemon(struct ublk_dev *dev, bool recovery)
{
	int ret, i;
	void *thread_ret;
	const struct ublksrv_ctrl_dev_info *dinfo = &dev->dev_info;

	daemon(1, 1);

	ublk_dbg(UBLK_DBG_DEV, "%s enter\n", __func__);

	ret = ublk_dev_prep(dev);
	if (ret)
		return ret;

	for (i = 0; i < dinfo->nr_hw_queues; i++) {
		dev->q[i].dev = dev;
		dev->q[i].q_id = i;
		pthread_create(&dev->q[i].thread, NULL,
				ublk_io_handler_fn,
				&dev->q[i]);
	}


	/* everything is fine now, start us */
	if (recovery) {
		ret = ublk_ctrl_end_user_recover(dev, getpid());
		if (ret < 0) {
			ublk_err("%s: ublk_ctrl_end_user_recover failed: %d\n", __func__, ret);
			goto fail;
		}
	} else {
		ublk_set_parameters(dev);
		ret = ublk_ctrl_start_dev(dev, getpid());
		if (ret < 0) {
			ublk_err("%s: ublk_ctrl_start_dev failed: %d\n", __func__, ret);
			goto fail;
		}
	}

	ublk_ctrl_get_info(dev);
	ublk_ctrl_dump(dev, true);

	/* wait until we are terminated */
	for (i = 0; i < dinfo->nr_hw_queues; i++)
		pthread_join(dev->q[i].thread, &thread_ret);
 fail:
	ublk_dev_unprep(dev);
	ublk_dbg(UBLK_DBG_DEV, "%s exit\n", __func__);

	return ret;
}

static int cmd_dev_add(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "type",		1,	NULL, 't' },
		{ "number",		1,	NULL, 'n' },
		{ "queues",		1,	NULL, 'q' },
		{ "depth",		1,	NULL, 'd' },
		{ "recovery",		0,	NULL, 'r' },
		{ "debug_mask",	1,	NULL, 0},
		{ "quiet",	0,	NULL, 0},
		{ NULL }
	};
	const struct ublk_tgt_ops *ops;
	struct ublksrv_ctrl_dev_info *info;
	struct ublk_dev *dev;
	int ret, option_idx, opt;
	const char *tgt_type = NULL;
	int dev_id = -1;
	unsigned nr_queues = 2, depth = UBLK_QUEUE_DEPTH;
	int user_recovery = 0;

	while ((opt = getopt_long(argc, argv, "-:t:n:d:q:r",
				  longopts, &option_idx)) != -1) {
		switch (opt) {
		case 'n':
			dev_id = strtol(optarg, NULL, 10);
			break;
		case 't':
			tgt_type = optarg;
			break;
		case 'q':
			nr_queues = strtol(optarg, NULL, 10);
			break;
		case 'd':
			depth = strtol(optarg, NULL, 10);
			break;
		case 'r':
			user_recovery = 1;
			break;
		case 0:
			if (!strcmp(longopts[option_idx].name, "debug_mask"))
				ublk_dbg_mask = strtol(optarg, NULL, 16);
			if (!strcmp(longopts[option_idx].name, "quiet"))
				ublk_dbg_mask = 0;
			break;
		}
	}

	optind = 0;

	ops = ublk_find_tgt(tgt_type);
	if (!ops) {
		ublk_err("%s: no such tgt type, type %s\n",
				__func__, tgt_type);
		return -ENODEV;
	}

	if (nr_queues > UBLK_MAX_QUEUES || depth > UBLK_QUEUE_DEPTH) {
		ublk_err("%s: invalid nr_queues or depth queues %u depth %u\n",
				__func__, nr_queues, depth);
		return -EINVAL;
	}

	dev = ublk_ctrl_init();
	if (!dev) {
		ublk_err("%s: can't alloc dev id %d, type %s\n",
				__func__, dev_id, tgt_type);
		return -ENOMEM;
	}

	info = &dev->dev_info;
	info->dev_id = dev_id;
        info->nr_hw_queues = nr_queues;
        info->queue_depth = depth;
	if (user_recovery)
		info->flags |= UBLK_F_USER_RECOVERY;
	dev->tgt.ops = ops;
	dev->tgt.argc = argc;
	dev->tgt.argv = argv;

	ret = ublk_ctrl_add_dev(dev);
	if (ret < 0) {
		ublk_err("%s: can't add dev id %d, type %s ret %d\n",
				__func__, dev_id, tgt_type, ret);
		goto fail;
	}

	ret = ublk_start_daemon(dev, false);
	if (ret < 0) {
		ublk_err("%s: can't start daemon id %d, type %s\n",
				__func__, dev_id, tgt_type);
		goto fail_del;
	}

fail_del:
	ublk_ctrl_del_dev(dev);
fail:
	ublk_ctrl_deinit(dev);
	return ret;
}

static int cmd_dev_recover(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "type",		1,	NULL, 't' },
		{ "number",		1,	NULL, 'n' },
		{ "debug_mask",	1,	NULL, 0},
		{ "quiet",	0,	NULL, 0},
		{ NULL }
	};
	const struct ublk_tgt_ops *ops;
	struct ublksrv_ctrl_dev_info *info;
	struct ublk_dev *dev;
	int ret, option_idx, opt;
	const char *tgt_type = NULL;
	int dev_id = -1;

	while ((opt = getopt_long(argc, argv, "-:t:n:d:q:",
				  longopts, &option_idx)) != -1) {
		switch (opt) {
		case 'n':
			dev_id = strtol(optarg, NULL, 10);
			break;
		case 't':
			tgt_type = optarg;
			break;
		case 0:
			if (!strcmp(longopts[option_idx].name, "debug_mask"))
				ublk_dbg_mask = strtol(optarg, NULL, 16);
			if (!strcmp(longopts[option_idx].name, "quiet"))
				ublk_dbg_mask = 0;
			break;
		}
	}

	optind = 0;

	ops = ublk_find_tgt(tgt_type);
	if (!ops) {
		ublk_err("%s: no such tgt type, type %s\n",
				__func__, tgt_type);
		return -ENODEV;
	}

	dev = ublk_ctrl_init();
	if (!dev) {
		ublk_err("%s: can't alloc dev id %d, type %s\n",
				__func__, dev_id, tgt_type);
		return -ENOMEM;
	}

	info = &dev->dev_info;
	info->dev_id = dev_id;
	ret = ublk_ctrl_get_info(dev);
	if (ret < 0) {
		ublk_err("%s: can't get dev info from %d\n", __func__, dev_id);
		goto fail;
	}

	ret = ublk_ctrl_get_params(dev, &dev->tgt.params);
	if (ret) {
		ublk_err("dev %d set basic parameter failed %d\n",
				dev->dev_info.dev_id, ret);
		goto fail;
	}

	dev->tgt.ops = ops;
	dev->tgt.argc = argc;
	dev->tgt.argv = argv;
	ret = ublk_ctrl_start_user_recover(dev);
	if (ret < 0) {
		ublk_err("%s: can't start recovery for %d\n", __func__, dev_id);
		goto fail;
	}

	ret = ublk_start_daemon(dev, true);
	if (ret < 0) {
		ublk_err("%s: can't start daemon id %d, type %s\n",
				__func__, dev_id, tgt_type);
		goto fail_del;
	}
fail_del:
	ublk_ctrl_del_dev(dev);
fail:
	ublk_ctrl_deinit(dev);
	return ret;
}

static int ublk_stop_io_daemon(const struct ublk_dev *dev)
{
	int daemon_pid = dev->dev_info.ublksrv_pid;
	int cnt = 0, ret;

	if (daemon_pid == -1)
		return 0;

	/* wait until daemon is exited, or timeout after 3 seconds */
	do {
		ret = kill(daemon_pid, 0);
		if (ret)
			break;
		usleep(500000);
		cnt++;
	} while (!ret && cnt < 6);

	ublk_dbg(UBLK_DBG_DEV, "%s: pid %d ret %d\n", __func__, daemon_pid, ret);

	return ret != 0 ? 0 : -1;
}

static int __cmd_dev_del(int number, bool log)
{
	struct ublk_dev *dev;
	int ret = -ENODEV;

	dev = ublk_ctrl_init();
	if (!dev) {
		ublk_err("del dev %d failed\n", number);
		goto fail;
	}

	dev->dev_info.dev_id = number;

	ret = ublk_ctrl_get_info(dev);
	if (ret < 0) {
		goto fail;
	}

	ret = ublk_ctrl_stop_dev(dev);
	if (ret < 0) {
		if (log)
			ublk_err("stop dev %d failed\n", number);
		goto fail;
	}

	ret = ublk_stop_io_daemon(dev);
	if (ret < 0) {
		if (log)
			ublk_err("stop daemon %d failed\n", number);
	}

	ublk_ctrl_del_dev(dev);
fail:
	ublk_ctrl_deinit(dev);
	return ret;
}

static int cmd_dev_del(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "number",		1,	NULL, 'n' },
		{ "all",		0,	NULL, 'a' },
		{ "debug_mask",	1,	NULL, 0},
		{ NULL }
	};
	int number = -2;
	int opt, i, option_idx;

	while ((opt = getopt_long(argc, argv, "n:a",
				  longopts, &option_idx)) != -1) {
		switch (opt) {
		case 'a':
			number = -1;
			break;

		case 'n':
			number = strtol(optarg, NULL, 10);
			break;
		case 0:
			if (!strcmp(longopts[option_idx].name, "debug_mask"))
				ublk_dbg_mask = strtol(optarg, NULL, 16);
			break;
		}
	}

	if (number >= 0)
		return __cmd_dev_del(number, true);
	else if (number != -1) {
		ublk_err("%s: pass wrong devid or not delete via -a\n");
		return -EINVAL;
	}

	for (i = 0; i < 255; i++)
		__cmd_dev_del(i, false);

	return 0;
}

static int __cmd_dev_list(int number, bool log)
{
	struct ublk_dev *dev;
	int ret = -ENODEV;

	dev = ublk_ctrl_init();
	if (!dev) {
		ublk_err("list dev %d failed\n", number);
		goto exit;
	}

	dev->dev_info.dev_id = number;

	ret = ublk_ctrl_get_info(dev);
	if (ret < 0) {
		if (log)
			ublk_err("%s: can't get dev info from %d: %d\n",
					__func__, number, ret);
	} else {
		ublk_ctrl_dump(dev, false);
	}

	ublk_ctrl_deinit(dev);
exit:
	return ret;
}


static int cmd_dev_list(int argc, char *argv[])
{
	static const struct option longopts[] = {
		{ "number",		1,	NULL, 'n' },
		{ "all",		0,	NULL, 'a' },
		{ NULL }
	};
	int number = -1;
	int opt, i;

	while ((opt = getopt_long(argc, argv, "n:a",
				  longopts, NULL)) != -1) {
		switch (opt) {
		case 'a':
			break;

		case 'n':
			number = strtol(optarg, NULL, 10);
			break;
		}
	}

	if (number >= 0)
		return __cmd_dev_list(number, true);

	for (i = 0; i < 255; i++)
		__cmd_dev_list(i, false);

	return 0;
}

static int cmd_dev_help(int argc, char *argv[])
{
	printf("%s add -t {null|loop} [-q nr_queues] [-d depth] [-n dev_id] \n",
			argv[0]);
	printf("\t default: nr_queues=2(max 4), depth=128(max 128), dev_id=-1(auto allocation)\n");
	printf("\t -t loop -f backing_file \n");
	printf("\t -t null\n");
	printf("%s del [-n dev_id] -a \n", argv[0]);
	printf("\t -a delete all devices -n delete specified device\n");
	printf("%s list [-n dev_id] -a \n", argv[0]);
	printf("\t -a list all devices, -n list specified device, default -a \n");
	printf("%s recover -t {null|loop} [-n dev_id] \n", argv[0]);
	printf("\t -t loop -f backing_file \n");
	printf("\t -t null\n");
	return 0;
}

static int ublk_null_tgt_init(struct ublk_dev *dev)
{
	const struct ublksrv_ctrl_dev_info *info = &dev->dev_info;
	unsigned long dev_size = 250UL << 30;

	dev->tgt.dev_size = dev_size;
	dev->tgt.params = (struct ublk_params) {
		.types = UBLK_PARAM_TYPE_BASIC,
		.basic = {
			.logical_bs_shift	= 9,
			.physical_bs_shift	= 12,
			.io_opt_shift		= 12,
			.io_min_shift		= 9,
			.max_sectors		= info->max_io_buf_bytes >> 9,
			.dev_sectors		= dev_size >> 9,
		},
	};

	return 0;
}

static int ublk_null_tgt_recover(struct ublk_dev *dev)
{
	dev->tgt.dev_size = dev->tgt.params.basic.dev_sectors << 9;
	return 0;
}

static int ublk_null_queue_io(struct ublk_queue *q, int tag)
{
	const struct ublksrv_io_desc *iod = ublk_get_iod(q, tag);

	ublk_complete_io(q, tag, iod->nr_sectors << 9);

	return 0;
}

static int loop_queue_tgt_io(struct ublk_queue *q, int tag)
{
	const struct ublksrv_io_desc *iod = ublk_get_iod(q, tag);
	struct io_uring_sqe *sqe = io_uring_get_sqe(&q->ring);
	unsigned ublk_op = ublksrv_get_op(iod);

	if (!sqe)
		return -ENOMEM;

	switch (ublk_op) {
	case UBLK_IO_OP_FLUSH:
		io_uring_prep_sync_file_range(sqe, 1 /*fds[1]*/,
				iod->nr_sectors << 9,
				iod->start_sector << 9,
				IORING_FSYNC_DATASYNC);
		io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		break;
	case UBLK_IO_OP_WRITE_ZEROES:
	case UBLK_IO_OP_DISCARD:
		return -ENOTSUP;
	case UBLK_IO_OP_READ:
		io_uring_prep_read(sqe, 1 /*fds[1]*/,
				(void *)iod->addr,
				iod->nr_sectors << 9,
				iod->start_sector << 9);
		io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		break;
	case UBLK_IO_OP_WRITE:
		io_uring_prep_write(sqe, 1 /*fds[1]*/,
				(void *)iod->addr,
				iod->nr_sectors << 9,
				iod->start_sector << 9);
		io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		break;
	default:
		return -EINVAL;
	}

	q->io_inflight++;
	/* bit63 marks us as tgt io */
	sqe->user_data = build_user_data(tag, ublk_op, 0, 1);

	ublk_dbg(UBLK_DBG_IO, "%s: tag %d ublk io %x %llx %u\n", __func__, tag,
			iod->op_flags, iod->start_sector, iod->nr_sectors << 9);
	return 1;
}

static int ublk_loop_queue_io(struct ublk_queue *q, int tag)
{
	int queued = loop_queue_tgt_io(q, tag);

	if (queued < 0)
		ublk_complete_io(q, tag, queued);

	return 0;
}

static void ublk_loop_io_done(struct ublk_queue *q, int tag,
		const struct io_uring_cqe *cqe)
{
	int cqe_tag = user_data_to_tag(cqe->user_data);

	ublk_assert(tag == cqe_tag);
	ublk_complete_io(q, tag, cqe->res);
	q->io_inflight--;
}

static void ublk_loop_tgt_deinit(struct ublk_dev *dev)
{
	fsync(dev->fds[1]);
	close(dev->fds[1]);
}

static int ublk_loop_tgt_init(struct ublk_dev *dev)
{
	static const struct option lo_longopts[] = {
		{ "file",		1,	NULL, 'f' },
		{ NULL }
	};
	unsigned long long bytes;
	char **argv = dev->tgt.argv;
	int argc = dev->tgt.argc;
	char *file = NULL;
	struct stat st;
	int fd, opt;
	struct ublk_params p = {
		.types = UBLK_PARAM_TYPE_BASIC,
		.basic = {
			.logical_bs_shift	= 9,
			.physical_bs_shift	= 12,
			.io_opt_shift	= 12,
			.io_min_shift	= 9,
			.max_sectors = dev->dev_info.max_io_buf_bytes >> 9,
		},
	};

	while ((opt = getopt_long(argc, argv, "-:f:",
				  lo_longopts, NULL)) != -1) {
		switch (opt) {
		case 'f':
			file = strdup(optarg);
			break;
		}
	}

	if (!file) {
		ublk_err( "%s: backing file is unset!\n", __func__);
		return -EINVAL;
	}

	ublk_dbg(UBLK_DBG_DEV, "%s: file %s\n", __func__, file);

	fd = open(file, O_RDWR);
	if (fd < 0) {
		ublk_err("%s: backing file %s can't be opened: %s\n",
				__func__, file, strerror(errno));
		return -EBADF;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		return -EBADF;
	}

	if (S_ISBLK(st.st_mode)) {
		unsigned int bs, pbs;

		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
			return -EBADF;
		if (ioctl(fd, BLKSSZGET, &bs) != 0)
			return -1;
		if (ioctl(fd, BLKPBSZGET, &pbs) != 0)
			return -1;
		p.basic.logical_bs_shift = ilog2(bs);
		p.basic.physical_bs_shift = ilog2(pbs);
	} else if (S_ISREG(st.st_mode)) {
		bytes = st.st_size;
		p.basic.logical_bs_shift = ilog2(st.st_blksize);
		p.basic.physical_bs_shift = ilog2(st.st_blksize);
	} else {
		bytes = 0;
	}

	if (fcntl(fd, F_SETFL, O_DIRECT)) {
		p.basic.logical_bs_shift = 9;
		p.basic.physical_bs_shift = 12;
		ublk_log("%s: %s, ublk-loop fallback to buffered IO\n",
				__func__, strerror(errno));
	}

	dev->tgt.dev_size = bytes;
	p.basic.dev_sectors = bytes >> 9;
	dev->fds[1] = fd;
	dev->nr_fds += 1;
	dev->tgt.params = p;

	return 0;
}

static int ublk_loop_tgt_recover(struct ublk_dev *dev)
{
	static const struct option lo_longopts[] = {
		{ "file",		1,	NULL, 'f' },
		{ NULL }
	};
	unsigned long long bytes;
	char **argv = dev->tgt.argv;
	int argc = dev->tgt.argc;
	struct ublk_params p = dev->tgt.params;
	char *file = NULL;
	int fd, opt;
	unsigned int bs = 1 << 9, pbs = 1 << 12;
	struct stat st;

	while ((opt = getopt_long(argc, argv, "-:f:",
				  lo_longopts, NULL)) != -1) {
		switch (opt) {
		case 'f':
			file = strdup(optarg);
			break;
		}
	}

	if (!file) {
		ublk_err( "%s: backing file is unset!\n", __func__);
		return -EINVAL;
	}

	ublk_dbg(UBLK_DBG_DEV, "%s: file %s\n", __func__, file);

	fd = open(file, O_RDWR);
	if (fd < 0) {
		ublk_err( "%s: backing file %s can't be opened: %s\n",
				__func__, file, strerror(errno));
		return -EBADF;
	}

	if (fstat(fd, &st) < 0) {
		close(fd);
		return -EBADF;
	}

	if (S_ISBLK(st.st_mode)) {
		if (ioctl(fd, BLKGETSIZE64, &bytes) != 0)
			return -EBADF;
		if (ioctl(fd, BLKSSZGET, &bs) != 0)
			return -1;
		if (ioctl(fd, BLKPBSZGET, &pbs) != 0)
			return -1;
	} else if (S_ISREG(st.st_mode)) {
		bytes = st.st_size;
		bs = st.st_blksize;
		pbs = st.st_blksize;
	} else {
		bytes = 0;
	}

	if (fcntl(fd, F_SETFL, O_DIRECT)) {
		/* buffered I/O */
		ublk_log("%s: %s, ublk-loop fallback to buffered IO\n",
				__func__, strerror(errno));
	}
	else {
		/* direct I/O */
		if (p.basic.logical_bs_shift != ilog2(bs)) {
			ublk_err("%s: logical block size should be %d, I got %d\n",
					__func__, 1 << p.basic.logical_bs_shift, bs);
			return -1;
		}
		if (p.basic.physical_bs_shift != ilog2(pbs)) {
			ublk_err("%s: physical block size should be %d, I got %d\n",
					__func__, 1 << p.basic.physical_bs_shift, pbs);
			return -1;
		}
	}

	if (p.basic.dev_sectors << 9 != bytes) {
		ublk_err("%s: device size should be %lld, I got %lld\n",
				__func__, p.basic.dev_sectors << 9, bytes);
		return -1;
	}

	dev->tgt.dev_size = bytes;
	dev->fds[1] = fd;
	dev->nr_fds += 1;

	return 0;
}

const struct ublk_tgt_ops tgt_ops_list[] = {
	{
		.name = "null",
		.init_tgt = ublk_null_tgt_init,
		.queue_io = ublk_null_queue_io,
		.recover_tgt = ublk_null_tgt_recover,
	},

	{
		.name = "loop",
		.init_tgt = ublk_loop_tgt_init,
		.deinit_tgt = ublk_loop_tgt_deinit,
		.queue_io = ublk_loop_queue_io,
		.tgt_io_done = ublk_loop_io_done,
		.recover_tgt = ublk_loop_tgt_recover,
	},
};

static const struct ublk_tgt_ops *ublk_find_tgt(const char *name)
{
	const struct ublk_tgt_ops *ops;
	int i;

	if (name == NULL)
		return NULL;

	for (i = 0; sizeof(tgt_ops_list) / sizeof(*ops); i++)
		if (strcmp(tgt_ops_list[i].name, name) == 0)
			return &tgt_ops_list[i];
	return NULL;
}

int main(int argc, char *argv[])
{
	const char *cmd = argv[1];
	int ret = -EINVAL;

	if (argc == 1)
		goto out;

	if (!strcmp(cmd, "add"))
		ret = cmd_dev_add(argc, argv);
	else if (!strcmp(cmd, "del"))
		ret = cmd_dev_del(argc, argv);
	else if (!strcmp(cmd, "list"))
		ret = cmd_dev_list(argc, argv);
	else if (!strcmp(cmd, "help"))
		ret = cmd_dev_help(argc, argv);
	else if (!strcmp(cmd, "recover"))
		ret = cmd_dev_recover(argc, argv);
out:
	if (ret)
		cmd_dev_help(argc, argv);

	return ret;
}
