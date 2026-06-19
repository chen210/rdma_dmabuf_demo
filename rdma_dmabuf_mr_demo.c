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
#include <sys/ioctl.h>

#include <infiniband/verbs.h>
#include <drm/drm.h>
#include <drm/amdgpu_drm.h>

typedef struct ibv_mr *(*ibv_reg_dmabuf_mr_fn)(struct ibv_pd *pd,
					       uint64_t offset,
					       size_t length,
					       uint64_t iova,
					       int fd,
					       int access);

struct options {
	const char *device;
	const char *amdgpu_drm_path;
	int dmabuf_fd;
	int use_normal_mr;
	size_t length;
	uint64_t offset;
	uint64_t iova;
	uint64_t amdgpu_domain;
	uint64_t amdgpu_flags;
};

struct exported_dmabuf {
	int drm_fd;
	int dmabuf_fd;
	uint32_t gem_handle;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s --device mlx5_0 --normal --length 4096\n"
		"  %s --device mlx5_0 --dmabuf-fd <fd> --length <bytes> [--offset 0] [--iova 0]\n\n"
		"  %s --device mlx5_0 --amdgpu-drm /dev/dri/renderD128 --length <bytes> [--amdgpu-domain vram|gtt|cpu] [--iova 0]\n\n"
		"Notes:\n"
		"  --normal registers process memory with ibv_reg_mr().\n"
		"  --dmabuf-fd registers an existing dma-buf fd with ibv_reg_dmabuf_mr().\n"
		"  --amdgpu-drm creates an AMDGPU GEM BO, exports it as dma-buf, then registers it.\n"
		"  A numeric fd is only valid inside the current process unless passed with SCM_RIGHTS.\n",
		prog, prog, prog);
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

static int parse_amdgpu_domain(const char *s, uint64_t *domain)
{
	if (!strcmp(s, "vram")) {
		*domain = AMDGPU_GEM_DOMAIN_VRAM;
		return 0;
	}
	if (!strcmp(s, "gtt")) {
		*domain = AMDGPU_GEM_DOMAIN_GTT;
		return 0;
	}
	if (!strcmp(s, "cpu")) {
		*domain = AMDGPU_GEM_DOMAIN_CPU;
		return 0;
	}
	return parse_u64(s, domain);
}

static int parse_options(int argc, char **argv, struct options *opt)
{
	memset(opt, 0, sizeof(*opt));
	opt->dmabuf_fd = -1;
	opt->length = 4096;
	opt->amdgpu_domain = AMDGPU_GEM_DOMAIN_VRAM;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			opt->device = argv[++i];
		} else if (!strcmp(argv[i], "--amdgpu-drm") && i + 1 < argc) {
			opt->amdgpu_drm_path = argv[++i];
		} else if (!strcmp(argv[i], "--amdgpu-domain") && i + 1 < argc) {
			if (parse_amdgpu_domain(argv[++i], &opt->amdgpu_domain))
				return -1;
		} else if (!strcmp(argv[i], "--amdgpu-flags") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opt->amdgpu_flags))
				return -1;
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

	int modes = 0;
	modes += opt->use_normal_mr ? 1 : 0;
	modes += opt->dmabuf_fd >= 0 ? 1 : 0;
	modes += opt->amdgpu_drm_path ? 1 : 0;
	if (modes != 1)
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

static void close_exported_dmabuf(struct exported_dmabuf *exp)
{
	if (exp->drm_fd >= 0 && exp->gem_handle) {
		struct drm_gem_close close_arg;

		memset(&close_arg, 0, sizeof(close_arg));
		close_arg.handle = exp->gem_handle;
		if (ioctl(exp->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg))
			perror("DRM_IOCTL_GEM_CLOSE");
		exp->gem_handle = 0;
	}
	if (exp->dmabuf_fd >= 0) {
		close(exp->dmabuf_fd);
		exp->dmabuf_fd = -1;
	}
	if (exp->drm_fd >= 0) {
		close(exp->drm_fd);
		exp->drm_fd = -1;
	}
}

static int get_drm_driver_name(int fd, char *name, size_t name_len)
{
	struct drm_version version;

	if (!name || name_len == 0)
		return -1;

	memset(name, 0, name_len);
	memset(&version, 0, sizeof(version));
	version.name = name;
	version.name_len = name_len - 1;

	if (ioctl(fd, DRM_IOCTL_VERSION, &version)) {
		perror("DRM_IOCTL_VERSION");
		return -1;
	}

	name[name_len - 1] = '\0';
	return 0;
}

static int export_amdgpu_bo(const struct options *opt, struct exported_dmabuf *exp)
{
	union drm_amdgpu_gem_create create_arg;
	struct drm_prime_handle prime_arg;
	char driver_name[64];

	memset(exp, 0, sizeof(*exp));
	exp->drm_fd = -1;
	exp->dmabuf_fd = -1;

	exp->drm_fd = open(opt->amdgpu_drm_path, O_RDWR | O_CLOEXEC);
	if (exp->drm_fd < 0) {
		perror("open amdgpu drm node");
		return 1;
	}

	if (get_drm_driver_name(exp->drm_fd, driver_name, sizeof(driver_name))) {
		close_exported_dmabuf(exp);
		return 1;
	}

	if (strcmp(driver_name, "amdgpu")) {
		fprintf(stderr,
			"DRM node driver is '%s', expected 'amdgpu'.\n"
			"AMDGPU GEM_CREATE/PRIME exporter mode cannot run on this node.\n",
			driver_name[0] ? driver_name : "<unknown>");
		close_exported_dmabuf(exp);
		return 1;
	}

	memset(&create_arg, 0, sizeof(create_arg));
	create_arg.in.bo_size = opt->length;
	create_arg.in.alignment = 4096;
	create_arg.in.domains = opt->amdgpu_domain;
	create_arg.in.domain_flags = opt->amdgpu_flags;

	if (ioctl(exp->drm_fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &create_arg)) {
		perror("DRM_IOCTL_AMDGPU_GEM_CREATE");
		close_exported_dmabuf(exp);
		return 1;
	}
	exp->gem_handle = create_arg.out.handle;

	memset(&prime_arg, 0, sizeof(prime_arg));
	prime_arg.handle = exp->gem_handle;
	prime_arg.flags = DRM_CLOEXEC | DRM_RDWR;

	if (ioctl(exp->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_arg)) {
		perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
		close_exported_dmabuf(exp);
		return 1;
	}
	exp->dmabuf_fd = prime_arg.fd;

	printf("amdgpu BO exported as dma-buf\n");
	printf("drm_node=%s gem_handle=%u dmabuf_fd=%d length=%zu domain=0x%" PRIx64
	       " flags=0x%" PRIx64 "\n",
	       opt->amdgpu_drm_path, exp->gem_handle, exp->dmabuf_fd,
	       opt->length, opt->amdgpu_domain, opt->amdgpu_flags);

	return 0;
}

int main(int argc, char **argv)
{
	struct options opt;
	struct exported_dmabuf exp;
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
	else if (opt.amdgpu_drm_path) {
		struct options dmabuf_opt = opt;

		ret = export_amdgpu_bo(&opt, &exp);
		if (!ret) {
			dmabuf_opt.dmabuf_fd = exp.dmabuf_fd;
			dmabuf_opt.offset = 0;
			ret = register_dmabuf_mr(pd, &dmabuf_opt);
			close_exported_dmabuf(&exp);
		}
	} else {
		ret = register_dmabuf_mr(pd, &opt);
	}

	if (ibv_dealloc_pd(pd))
		perror("ibv_dealloc_pd");
	if (ibv_close_device(ctx))
		perror("ibv_close_device");

	return ret;
}
