#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#include <infiniband/verbs.h>

typedef struct ibv_mr *(*ibv_reg_dmabuf_mr_fn)(struct ibv_pd *pd,
					       uint64_t offset,
					       size_t length,
					       uint64_t iova,
					       int fd,
					       int access);

struct options {
	const char *device;
	int dmabuf_fd;
	int use_normal_mr;
	size_t length;
	uint64_t offset;
	uint64_t iova;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s --device mlx5_0 --normal --length 4096\n"
		"  %s --device mlx5_0 --dmabuf-fd <fd> --length <bytes> [--offset 0] [--iova 0]\n\n"
		"Notes:\n"
		"  --normal registers process memory with ibv_reg_mr().\n"
		"  --dmabuf-fd registers an existing dma-buf fd with ibv_reg_dmabuf_mr().\n"
		"  A numeric fd is only valid inside the current process unless passed with SCM_RIGHTS.\n",
		prog, prog);
}

static int parse_u64(const char *s, uint64_t *out)
{
	char *end = NULL;
	errno = 0;
	*out = strtoull(s, &end, 0);
	if (errno || !end || *end)
		return -1;
	return 0;
}

static int parse_options(int argc, char **argv, struct options *opt)
{
	memset(opt, 0, sizeof(*opt));
	opt->dmabuf_fd = -1;
	opt->length = 4096;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			opt->device = argv[++i];
		} else if (!strcmp(argv[i], "--dmabuf-fd") && i + 1 < argc) {
			uint64_t v;
			if (parse_u64(argv[++i], &v) || v > INT32_MAX)
				return -1;
			opt->dmabuf_fd = (int)v;
		} else if (!strcmp(argv[i], "--normal")) {
			opt->use_normal_mr = 1;
		} else if (!strcmp(argv[i], "--length") && i + 1 < argc) {
			uint64_t v;
			if (parse_u64(argv[++i], &v) || v == 0)
				return -1;
			opt->length = (size_t)v;
		} else if (!strcmp(argv[i], "--offset") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opt->offset))
				return -1;
		} else if (!strcmp(argv[i], "--iova") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opt->iova))
				return -1;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			return 1;
		} else {
			return -1;
		}
	}

	if (!opt->device)
		return -1;

	if (opt->use_normal_mr == (opt->dmabuf_fd >= 0))
		return -1;

	return 0;
}

static struct ibv_context *open_device_by_name(const char *name)
{
	struct ibv_device **list;
	struct ibv_context *ctx = NULL;
	int num = 0;

	list = ibv_get_device_list(&num);
	if (!list) {
		perror("ibv_get_device_list");
		return NULL;
	}

	for (int i = 0; i < num; i++) {
		const char *dev_name = ibv_get_device_name(list[i]);
		if (dev_name && !strcmp(dev_name, name)) {
			ctx = ibv_open_device(list[i]);
			if (!ctx)
				perror("ibv_open_device");
			break;
		}
	}

	ibv_free_device_list(list);
	return ctx;
}

static int fd_is_valid(int fd)
{
	return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

static ibv_reg_dmabuf_mr_fn find_reg_dmabuf_mr(void)
{
	void *sym;

	dlerror();
	sym = dlsym(RTLD_DEFAULT, "ibv_reg_dmabuf_mr");
	if (!sym)
		sym = dlsym(RTLD_DEFAULT, "ibv_reg_dmabuf_mr_iova");

	return (ibv_reg_dmabuf_mr_fn)sym;
}

static int register_normal_mr(struct ibv_pd *pd, size_t length)
{
	void *buf = NULL;
	struct ibv_mr *mr;
	int access = IBV_ACCESS_LOCAL_WRITE |
		     IBV_ACCESS_REMOTE_READ |
		     IBV_ACCESS_REMOTE_WRITE;

	if (posix_memalign(&buf, 4096, length)) {
		fprintf(stderr, "posix_memalign failed\n");
		return 1;
	}

	memset(buf, 0xa5, length);
	mr = ibv_reg_mr(pd, buf, length, access);
	if (!mr) {
		perror("ibv_reg_mr");
		free(buf);
		return 1;
	}

	printf("normal MR registered\n");
	printf("addr=%p length=%zu lkey=0x%x rkey=0x%x\n",
	       buf, length, mr->lkey, mr->rkey);

	if (ibv_dereg_mr(mr))
		perror("ibv_dereg_mr");
	free(buf);
	return 0;
}

static int register_dmabuf_mr(struct ibv_pd *pd, const struct options *opt)
{
	ibv_reg_dmabuf_mr_fn reg_dmabuf_mr;
	struct ibv_mr *mr;
	int access = IBV_ACCESS_LOCAL_WRITE |
		     IBV_ACCESS_REMOTE_READ |
		     IBV_ACCESS_REMOTE_WRITE;

	if (!fd_is_valid(opt->dmabuf_fd)) {
		fprintf(stderr, "invalid dma-buf fd %d in this process\n",
			opt->dmabuf_fd);
		return 1;
	}

	reg_dmabuf_mr = find_reg_dmabuf_mr();
	if (!reg_dmabuf_mr) {
		fprintf(stderr,
			"ibv_reg_dmabuf_mr symbol not found in libibverbs.\n"
			"Install a newer rdma-core/libibverbs or use provider direct ioctl.\n");
		return 1;
	}

	mr = reg_dmabuf_mr(pd, opt->offset, opt->length, opt->iova,
			   opt->dmabuf_fd, access);
	if (!mr) {
		perror("ibv_reg_dmabuf_mr");
		return 1;
	}

	printf("dmabuf MR registered\n");
	printf("fd=%d offset=%" PRIu64 " length=%zu iova=0x%" PRIx64
	       " lkey=0x%x rkey=0x%x\n",
	       opt->dmabuf_fd, opt->offset, opt->length, opt->iova,
	       mr->lkey, mr->rkey);

	if (ibv_dereg_mr(mr))
		perror("ibv_dereg_mr");
	return 0;
}

int main(int argc, char **argv)
{
	struct options opt;
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	int ret;

	ret = parse_options(argc, argv, &opt);
	if (ret) {
		usage(argv[0]);
		return ret > 0 ? 0 : 2;
	}

	ctx = open_device_by_name(opt.device);
	if (!ctx) {
		fprintf(stderr, "RDMA device not found or failed to open: %s\n",
			opt.device);
		return 1;
	}

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		perror("ibv_alloc_pd");
		ibv_close_device(ctx);
		return 1;
	}

	if (opt.use_normal_mr)
		ret = register_normal_mr(pd, opt.length);
	else
		ret = register_dmabuf_mr(pd, &opt);

	if (ibv_dealloc_pd(pd))
		perror("ibv_dealloc_pd");
	if (ibv_close_device(ctx))
		perror("ibv_close_device");

	return ret;
}
