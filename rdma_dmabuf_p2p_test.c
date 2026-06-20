#define _GNU_SOURCE

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <drm/drm.h>
#include <drm/amdgpu_drm.h>
#include <linux/dma-buf.h>

#define DEFAULT_TCP_PORT "18516"
#define DEFAULT_LENGTH 4096
#define DEFAULT_PATTERN_SEED 0x5a
#define CONTROL_MAGIC "RDMA_DMABUF_P2P_V1"

typedef struct ibv_mr *(*ibv_reg_dmabuf_mr_fn)(struct ibv_pd *pd,
					       uint64_t offset,
					       size_t length,
					       uint64_t iova,
					       int fd,
					       int access);

enum role {
	ROLE_UNSET = 0,
	ROLE_SERVER,
	ROLE_CLIENT,
};

struct options {
	enum role role;
	const char *server_host;
	const char *tcp_port;
	const char *device;
	int ib_port;
	int gid_index;
	enum ibv_mtu mtu;
	size_t length;
	uint8_t pattern_seed;
	const char *drm_node;
	uint64_t amdgpu_domain;
	uint64_t amdgpu_flags;
	uint64_t iova;
	const char *dmabuf_name;
	int skip_mmap_verify;
};

struct rdma_context {
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	struct ibv_port_attr port_attr;
	union ibv_gid gid;
	uint32_t psn;
};

struct peer_info {
	uint32_t qpn;
	uint32_t psn;
	uint32_t rkey;
	uint64_t vaddr;
	uint64_t length;
	uint16_t lid;
	union ibv_gid gid;
};

struct amdgpu_bo {
	int drm_fd;
	int dmabuf_fd;
	uint32_t gem_handle;
	uint64_t domain;
	uint64_t flags;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  Server, on the GPU/RDMA target node:\n"
		"    %s --server --device <rdma_dev> --ib-port 1 --gid-index <idx> \\\n"
		"      --drm-node /dev/dri/renderD128 --length 4096 [--tcp-port 18516]\n\n"
		"  Client, on the peer node:\n"
		"    %s --client <server_ip> --device <rdma_dev> --ib-port 1 \\\n"
		"      --gid-index <idx> --length 4096 [--tcp-port 18516]\n\n"
		"Options:\n"
		"  --amdgpu-domain vram|gtt|cpu|NUM  Server BO domain. Default: vram\n"
		"  --amdgpu-flags NUM               Server GEM_CREATE flags. Default: CPU_ACCESS_REQUIRED\n"
		"  --dmabuf-name NAME               Server dma-buf debug name. Default: generated\n"
		"  --iova NUM                       dmabuf MR IOVA/remote address. Default: 0\n"
		"  --mtu 1024|2048|4096             RC path MTU. Default: 1024\n"
		"  --pattern-seed NUM               Test pattern seed byte. Default: 0x5a\n"
		"  --skip-mmap-verify               Server skips CPU mmap content verification\n",
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

static int parse_mtu(const char *s, enum ibv_mtu *mtu)
{
	uint64_t value;

	if (parse_u64(s, &value))
		return -1;

	switch (value) {
	case 256:
		*mtu = IBV_MTU_256;
		return 0;
	case 512:
		*mtu = IBV_MTU_512;
		return 0;
	case 1024:
		*mtu = IBV_MTU_1024;
		return 0;
	case 2048:
		*mtu = IBV_MTU_2048;
		return 0;
	case 4096:
		*mtu = IBV_MTU_4096;
		return 0;
	default:
		return -1;
	}
}

static int parse_options(int argc, char **argv, struct options *opt)
{
	memset(opt, 0, sizeof(*opt));
	opt->tcp_port = DEFAULT_TCP_PORT;
	opt->ib_port = 1;
	opt->gid_index = -1;
	opt->mtu = IBV_MTU_1024;
	opt->length = DEFAULT_LENGTH;
	opt->pattern_seed = DEFAULT_PATTERN_SEED;
	opt->amdgpu_domain = AMDGPU_GEM_DOMAIN_VRAM;
	opt->amdgpu_flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	opt->iova = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--server")) {
			opt->role = ROLE_SERVER;
		} else if (!strcmp(argv[i], "--client") && i + 1 < argc) {
			opt->role = ROLE_CLIENT;
			opt->server_host = argv[++i];
		} else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			opt->device = argv[++i];
		} else if (!strcmp(argv[i], "--ib-port") && i + 1 < argc) {
			uint64_t v;
			if (parse_u64(argv[++i], &v) || v == 0 || v > INT32_MAX)
				return -1;
			opt->ib_port = (int)v;
		} else if (!strcmp(argv[i], "--gid-index") && i + 1 < argc) {
			uint64_t v;
			if (parse_u64(argv[++i], &v) || v > INT32_MAX)
				return -1;
			opt->gid_index = (int)v;
		} else if (!strcmp(argv[i], "--mtu") && i + 1 < argc) {
			if (parse_mtu(argv[++i], &opt->mtu))
				return -1;
		} else if (!strcmp(argv[i], "--length") && i + 1 < argc) {
			uint64_t v;
			if (parse_u64(argv[++i], &v) || v == 0)
				return -1;
			opt->length = (size_t)v;
		} else if (!strcmp(argv[i], "--pattern-seed") && i + 1 < argc) {
			uint64_t v;
			if (parse_u64(argv[++i], &v) || v > 0xff)
				return -1;
			opt->pattern_seed = (uint8_t)v;
		} else if (!strcmp(argv[i], "--drm-node") && i + 1 < argc) {
			opt->drm_node = argv[++i];
		} else if (!strcmp(argv[i], "--amdgpu-domain") && i + 1 < argc) {
			if (parse_amdgpu_domain(argv[++i], &opt->amdgpu_domain))
				return -1;
		} else if (!strcmp(argv[i], "--amdgpu-flags") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opt->amdgpu_flags))
				return -1;
		} else if (!strcmp(argv[i], "--dmabuf-name") && i + 1 < argc) {
			opt->dmabuf_name = argv[++i];
		} else if (!strcmp(argv[i], "--iova") && i + 1 < argc) {
			if (parse_u64(argv[++i], &opt->iova))
				return -1;
		} else if (!strcmp(argv[i], "--tcp-port") && i + 1 < argc) {
			opt->tcp_port = argv[++i];
		} else if (!strcmp(argv[i], "--skip-mmap-verify")) {
			opt->skip_mmap_verify = 1;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			exit(0);
		} else {
			return -1;
		}
	}

	if (opt->role == ROLE_UNSET || !opt->device || opt->gid_index < 0)
		return -1;
	if (opt->role == ROLE_SERVER && !opt->drm_node)
		return -1;
	if (opt->role == ROLE_CLIENT && !opt->server_host)
		return -1;
	if (opt->dmabuf_name && strlen(opt->dmabuf_name) >= DMA_BUF_NAME_LEN) {
		fprintf(stderr, "--dmabuf-name must be shorter than %d bytes\n",
			DMA_BUF_NAME_LEN);
		return -1;
	}

	return 0;
}

static uint8_t pattern_byte(size_t i, uint8_t seed)
{
	return (uint8_t)(seed + (i * 131u) + (i >> 7));
}

static void fill_pattern(uint8_t *buf, size_t length, uint8_t seed)
{
	for (size_t i = 0; i < length; i++)
		buf[i] = pattern_byte(i, seed);
}

static int verify_pattern(const uint8_t *buf, size_t length, uint8_t seed,
			  const char *where)
{
	for (size_t i = 0; i < length; i++) {
		uint8_t expected = pattern_byte(i, seed);

		if (buf[i] != expected) {
			fprintf(stderr,
				"%s pattern mismatch at offset %zu: got 0x%02x expected 0x%02x\n",
				where, i, buf[i], expected);
			return 1;
		}
	}

	printf("%s pattern verify passed, length=%zu seed=0x%02x\n",
	       where, length, seed);
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

static uint32_t random_psn(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((ts.tv_nsec ^ ts.tv_sec ^ getpid()) & 0xffffff);
}

static int init_rdma_context(const struct options *opt, struct rdma_context *r)
{
	struct ibv_qp_init_attr qp_init;

	memset(r, 0, sizeof(*r));

	r->ctx = open_device_by_name(opt->device);
	if (!r->ctx) {
		fprintf(stderr, "RDMA device not found or failed to open: %s\n",
			opt->device);
		return 1;
	}

	if (ibv_query_port(r->ctx, opt->ib_port, &r->port_attr)) {
		perror("ibv_query_port");
		return 1;
	}

	if (ibv_query_gid(r->ctx, opt->ib_port, opt->gid_index, &r->gid)) {
		perror("ibv_query_gid");
		return 1;
	}

	r->pd = ibv_alloc_pd(r->ctx);
	if (!r->pd) {
		perror("ibv_alloc_pd");
		return 1;
	}

	r->cq = ibv_create_cq(r->ctx, 8, NULL, NULL, 0);
	if (!r->cq) {
		perror("ibv_create_cq");
		return 1;
	}

	memset(&qp_init, 0, sizeof(qp_init));
	qp_init.send_cq = r->cq;
	qp_init.recv_cq = r->cq;
	qp_init.qp_type = IBV_QPT_RC;
	qp_init.cap.max_send_wr = 8;
	qp_init.cap.max_recv_wr = 1;
	qp_init.cap.max_send_sge = 1;
	qp_init.cap.max_recv_sge = 1;

	r->qp = ibv_create_qp(r->pd, &qp_init);
	if (!r->qp) {
		perror("ibv_create_qp");
		return 1;
	}

	r->psn = random_psn();

	printf("rdma context ready: device=%s port=%d gid_index=%d qpn=%u psn=%u lid=%u\n",
	       opt->device, opt->ib_port, opt->gid_index, r->qp->qp_num,
	       r->psn, r->port_attr.lid);
	return 0;
}

static void cleanup_rdma_context(struct rdma_context *r)
{
	if (r->qp) {
		if (ibv_destroy_qp(r->qp))
			perror("ibv_destroy_qp");
		r->qp = NULL;
	}
	if (r->cq) {
		if (ibv_destroy_cq(r->cq))
			perror("ibv_destroy_cq");
		r->cq = NULL;
	}
	if (r->pd) {
		if (ibv_dealloc_pd(r->pd))
			perror("ibv_dealloc_pd");
		r->pd = NULL;
	}
	if (r->ctx) {
		if (ibv_close_device(r->ctx))
			perror("ibv_close_device");
		r->ctx = NULL;
	}
}

static int modify_qp_to_init(struct ibv_qp *qp, const struct options *opt)
{
	struct ibv_qp_attr attr;
	int flags;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = opt->ib_port;
	attr.qp_access_flags = IBV_ACCESS_REMOTE_READ |
			       IBV_ACCESS_REMOTE_WRITE |
			       IBV_ACCESS_LOCAL_WRITE;

	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
		IBV_QP_ACCESS_FLAGS;

	if (ibv_modify_qp(qp, &attr, flags)) {
		perror("ibv_modify_qp INIT");
		return 1;
	}

	return 0;
}

static int connect_qp(struct rdma_context *r, const struct options *opt,
		      const struct peer_info *remote)
{
	struct ibv_qp_attr attr;
	int flags;

	if (modify_qp_to_init(r->qp, opt))
		return 1;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = opt->mtu;
	attr.dest_qp_num = remote->qpn;
	attr.rq_psn = remote->psn;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 1;
	attr.ah_attr.grh.dgid = remote->gid;
	attr.ah_attr.grh.sgid_index = opt->gid_index;
	attr.ah_attr.grh.hop_limit = 64;
	attr.ah_attr.dlid = remote->lid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = opt->ib_port;

	flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
		IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
		IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

	if (ibv_modify_qp(r->qp, &attr, flags)) {
		perror("ibv_modify_qp RTR");
		return 1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = r->psn;
	attr.max_rd_atomic = 1;

	flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
		IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

	if (ibv_modify_qp(r->qp, &attr, flags)) {
		perror("ibv_modify_qp RTS");
		return 1;
	}

	printf("qp connected: local_qpn=%u remote_qpn=%u remote_rkey=0x%x remote_vaddr=0x%" PRIx64 "\n",
	       r->qp->qp_num, remote->qpn, remote->rkey, remote->vaddr);
	return 0;
}

static void gid_to_hex(const union ibv_gid *gid, char out[33])
{
	static const char hex[] = "0123456789abcdef";

	for (int i = 0; i < 16; i++) {
		out[i * 2] = hex[(gid->raw[i] >> 4) & 0xf];
		out[i * 2 + 1] = hex[gid->raw[i] & 0xf];
	}
	out[32] = '\0';
}

static int hex_to_gid(const char *s, union ibv_gid *gid)
{
	if (strlen(s) != 32)
		return -1;

	memset(gid, 0, sizeof(*gid));
	for (int i = 0; i < 16; i++) {
		unsigned int byte;

		if (sscanf(s + i * 2, "%2x", &byte) != 1)
			return -1;
		gid->raw[i] = (uint8_t)byte;
	}

	return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = send(fd, p, len, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("send");
			return 1;
		}
		if (n == 0) {
			fprintf(stderr, "send returned 0\n");
			return 1;
		}
		p += n;
		len -= (size_t)n;
	}

	return 0;
}

static int read_line(int fd, char *buf, size_t len)
{
	size_t off = 0;

	if (!len)
		return 1;

	while (off + 1 < len) {
		char c;
		ssize_t n = recv(fd, &c, 1, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("recv");
			return 1;
		}
		if (n == 0) {
			fprintf(stderr, "peer closed control socket\n");
			return 1;
		}
		buf[off++] = c;
		if (c == '\n')
			break;
	}

	buf[off] = '\0';
	return 0;
}

static int tcp_listen(const char *port)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *rp;
	int fd = -1;
	int one = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if (getaddrinfo(NULL, port, &hints, &res)) {
		perror("getaddrinfo");
		return -1;
	}

	for (rp = res; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (!bind(fd, rp->ai_addr, rp->ai_addrlen) && !listen(fd, 1))
			break;
		close(fd);
		fd = -1;
	}

	freeaddrinfo(res);
	if (fd < 0)
		perror("listen/bind");
	return fd;
}

static int tcp_connect(const char *host, const char *port)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *rp;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host, port, &hints, &res)) {
		perror("getaddrinfo");
		return -1;
	}

	for (rp = res; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;
		if (!connect(fd, rp->ai_addr, rp->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}

	freeaddrinfo(res);
	if (fd < 0)
		perror("connect");
	return fd;
}

static int send_peer_info(int fd, const struct peer_info *info)
{
	char gid_hex[33];
	char line[256];
	int n;

	gid_to_hex(&info->gid, gid_hex);
	n = snprintf(line, sizeof(line),
		     CONTROL_MAGIC " qpn=%u psn=%u rkey=%u vaddr=%" PRIu64
		     " length=%" PRIu64 " lid=%u gid=%s\n",
		     info->qpn, info->psn, info->rkey, info->vaddr,
		     info->length, info->lid, gid_hex);
	if (n < 0 || (size_t)n >= sizeof(line)) {
		fprintf(stderr, "peer info line too long\n");
		return 1;
	}

	return write_all(fd, line, (size_t)n);
}

static int recv_peer_info(int fd, struct peer_info *info)
{
	char line[256];
	char magic[32];
	char gid_hex[33];
	unsigned int lid;

	if (read_line(fd, line, sizeof(line)))
		return 1;

	memset(info, 0, sizeof(*info));
	memset(gid_hex, 0, sizeof(gid_hex));
	if (sscanf(line,
		   "%31s qpn=%u psn=%u rkey=%u vaddr=%" SCNu64
		   " length=%" SCNu64 " lid=%u gid=%32s",
		   magic, &info->qpn, &info->psn, &info->rkey,
		   &info->vaddr, &info->length, &lid, gid_hex) != 8) {
		fprintf(stderr, "failed to parse peer info: %s", line);
		return 1;
	}

	if (strcmp(magic, CONTROL_MAGIC) || lid > UINT16_MAX ||
	    hex_to_gid(gid_hex, &info->gid)) {
		fprintf(stderr, "invalid peer info: %s", line);
		return 1;
	}

	info->lid = (uint16_t)lid;
	return 0;
}

static int exchange_peer_info(int fd, const struct peer_info *local,
			      struct peer_info *remote)
{
	if (send_peer_info(fd, local))
		return 1;
	if (recv_peer_info(fd, remote))
		return 1;
	return 0;
}

static int control_barrier(int fd, const char *tag)
{
	char line[64];
	char out[64];
	int n;

	n = snprintf(out, sizeof(out), "%s\n", tag);
	if (n < 0 || (size_t)n >= sizeof(out))
		return 1;

	if (write_all(fd, out, (size_t)n))
		return 1;
	if (read_line(fd, line, sizeof(line)))
		return 1;
	if (strcmp(line, out)) {
		fprintf(stderr, "control barrier expected %s, got %s", out, line);
		return 1;
	}

	return 0;
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

static int get_drm_driver_name(int fd, char *name, size_t name_len)
{
	struct drm_version version;

	if (!name || !name_len)
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

static void close_amdgpu_bo(struct amdgpu_bo *bo)
{
	if (bo->drm_fd >= 0 && bo->gem_handle) {
		struct drm_gem_close close_arg;

		memset(&close_arg, 0, sizeof(close_arg));
		close_arg.handle = bo->gem_handle;
		if (ioctl(bo->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg))
			perror("DRM_IOCTL_GEM_CLOSE");
		bo->gem_handle = 0;
	}
	if (bo->dmabuf_fd >= 0) {
		close(bo->dmabuf_fd);
		bo->dmabuf_fd = -1;
	}
	if (bo->drm_fd >= 0) {
		close(bo->drm_fd);
		bo->drm_fd = -1;
	}
}

static void set_dmabuf_debug_name(const struct options *opt,
				  const struct amdgpu_bo *bo)
{
	char generated[DMA_BUF_NAME_LEN];
	const char *name = opt->dmabuf_name;

	if (!name) {
		snprintf(generated, sizeof(generated), "rdma-p2p-%ld-%u",
			 (long)getpid(), bo->gem_handle);
		name = generated;
	}

	if (ioctl(bo->dmabuf_fd, DMA_BUF_SET_NAME, name)) {
		fprintf(stderr, "warning: DMA_BUF_SET_NAME(%s) failed: %s\n",
			name, strerror(errno));
		return;
	}

	printf("dmabuf debug name set: pid=%ld fd=%d name=%s\n",
	       (long)getpid(), bo->dmabuf_fd, name);
	printf("dmabuf debug inspect: sudo cat /proc/%ld/fdinfo/%d; sudo cat /sys/kernel/debug/dma_buf/bufinfo\n",
	       (long)getpid(), bo->dmabuf_fd);
}

static int create_amdgpu_bo(const struct options *opt, struct amdgpu_bo *bo)
{
	union drm_amdgpu_gem_create create_arg;
	struct drm_prime_handle prime_arg;
	char driver_name[64];

	memset(bo, 0, sizeof(*bo));
	bo->drm_fd = -1;
	bo->dmabuf_fd = -1;
	bo->domain = opt->amdgpu_domain;
	bo->flags = opt->amdgpu_flags;

	bo->drm_fd = open(opt->drm_node, O_RDWR | O_CLOEXEC);
	if (bo->drm_fd < 0) {
		perror("open amdgpu drm node");
		return 1;
	}

	if (get_drm_driver_name(bo->drm_fd, driver_name, sizeof(driver_name))) {
		close_amdgpu_bo(bo);
		return 1;
	}

	if (strcmp(driver_name, "amdgpu")) {
		fprintf(stderr, "DRM node driver is '%s', expected 'amdgpu'\n",
			driver_name[0] ? driver_name : "<unknown>");
		close_amdgpu_bo(bo);
		return 1;
	}

	memset(&create_arg, 0, sizeof(create_arg));
	create_arg.in.bo_size = opt->length;
	create_arg.in.alignment = 4096;
	create_arg.in.domains = opt->amdgpu_domain;
	create_arg.in.domain_flags = opt->amdgpu_flags;

	if (ioctl(bo->drm_fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &create_arg)) {
		perror("DRM_IOCTL_AMDGPU_GEM_CREATE");
		close_amdgpu_bo(bo);
		return 1;
	}
	bo->gem_handle = create_arg.out.handle;

	memset(&prime_arg, 0, sizeof(prime_arg));
	prime_arg.handle = bo->gem_handle;
	prime_arg.flags = DRM_CLOEXEC | DRM_RDWR;

	if (ioctl(bo->drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_arg)) {
		perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
		close_amdgpu_bo(bo);
		return 1;
	}
	bo->dmabuf_fd = prime_arg.fd;
	set_dmabuf_debug_name(opt, bo);

	printf("amdgpu BO exported: drm_node=%s gem_handle=%u dmabuf_fd=%d length=%zu domain=0x%" PRIx64 " flags=0x%" PRIx64 "\n",
	       opt->drm_node, bo->gem_handle, bo->dmabuf_fd, opt->length,
	       bo->domain, bo->flags);
	return 0;
}

static int mmap_verify_amdgpu_bo(const struct amdgpu_bo *bo, size_t length,
				 uint8_t seed)
{
	union drm_amdgpu_gem_mmap mmap_arg;
	void *map;
	int ret;

	memset(&mmap_arg, 0, sizeof(mmap_arg));
	mmap_arg.in.handle = bo->gem_handle;

	if (ioctl(bo->drm_fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &mmap_arg)) {
		perror("DRM_IOCTL_AMDGPU_GEM_MMAP");
		return 1;
	}

	map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
		   bo->drm_fd, (off_t)mmap_arg.out.addr_ptr);
	if (map == MAP_FAILED) {
		perror("mmap amdgpu BO");
		return 1;
	}

	ret = verify_pattern((const uint8_t *)map, length, seed,
			     "server amdgpu BO mmap");

	if (munmap(map, length))
		perror("munmap amdgpu BO");
	return ret;
}

static int register_dmabuf_mr(struct ibv_pd *pd, int dmabuf_fd,
			      uint64_t iova, size_t length, struct ibv_mr **mr_out)
{
	ibv_reg_dmabuf_mr_fn reg_dmabuf_mr;
	int access = IBV_ACCESS_LOCAL_WRITE |
		     IBV_ACCESS_REMOTE_READ |
		     IBV_ACCESS_REMOTE_WRITE;

	reg_dmabuf_mr = find_reg_dmabuf_mr();
	if (!reg_dmabuf_mr) {
		fprintf(stderr, "ibv_reg_dmabuf_mr symbol not found in libibverbs\n");
		return 1;
	}

	*mr_out = reg_dmabuf_mr(pd, 0, length, iova, dmabuf_fd, access);
	if (!*mr_out) {
		perror("ibv_reg_dmabuf_mr");
		return 1;
	}

	printf("dmabuf MR registered: fd=%d iova=0x%" PRIx64 " length=%zu lkey=0x%x rkey=0x%x\n",
	       dmabuf_fd, iova, length, (*mr_out)->lkey, (*mr_out)->rkey);
	return 0;
}

static int register_normal_mr(struct ibv_pd *pd, size_t length,
			      uint8_t **buf_out, struct ibv_mr **mr_out)
{
	void *buf = NULL;
	int access = IBV_ACCESS_LOCAL_WRITE |
		     IBV_ACCESS_REMOTE_READ |
		     IBV_ACCESS_REMOTE_WRITE;

	if (posix_memalign(&buf, 4096, length)) {
		fprintf(stderr, "posix_memalign failed\n");
		return 1;
	}

	*mr_out = ibv_reg_mr(pd, buf, length, access);
	if (!*mr_out) {
		perror("ibv_reg_mr");
		free(buf);
		return 1;
	}

	*buf_out = buf;
	printf("normal MR registered: addr=%p length=%zu lkey=0x%x rkey=0x%x\n",
	       buf, length, (*mr_out)->lkey, (*mr_out)->rkey);
	return 0;
}

static int poll_completion(struct ibv_cq *cq, const char *op)
{
	struct ibv_wc wc;

	for (;;) {
		int n = ibv_poll_cq(cq, 1, &wc);

		if (n < 0) {
			fprintf(stderr, "ibv_poll_cq failed for %s\n", op);
			return 1;
		}
		if (n == 0)
			continue;
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "%s completion failed: status=%s (%d) vendor_err=0x%x\n",
				op, ibv_wc_status_str(wc.status), wc.status,
				wc.vendor_err);
			return 1;
		}
		printf("%s completion passed\n", op);
		return 0;
	}
}

static int post_rdma_op(struct ibv_qp *qp, enum ibv_wr_opcode opcode,
			void *local_addr, size_t length, uint32_t lkey,
			uint64_t remote_addr, uint32_t rkey, const char *op)
{
	struct ibv_sge sge;
	struct ibv_send_wr wr;
	struct ibv_send_wr *bad_wr = NULL;

	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)local_addr;
	sge.length = (uint32_t)length;
	sge.lkey = lkey;

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t)local_addr;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode = opcode;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.wr.rdma.remote_addr = remote_addr;
	wr.wr.rdma.rkey = rkey;

	if (ibv_post_send(qp, &wr, &bad_wr)) {
		perror(op);
		return 1;
	}

	return 0;
}

static void fill_local_info(const struct rdma_context *r, uint32_t rkey,
			    uint64_t vaddr, uint64_t length,
			    struct peer_info *info)
{
	memset(info, 0, sizeof(*info));
	info->qpn = r->qp->qp_num;
	info->psn = r->psn;
	info->rkey = rkey;
	info->vaddr = vaddr;
	info->length = length;
	info->lid = r->port_attr.lid;
	info->gid = r->gid;
}

static int run_server(const struct options *opt)
{
	struct rdma_context r;
	struct amdgpu_bo bo;
	struct ibv_mr *dmabuf_mr = NULL;
	struct peer_info local;
	struct peer_info remote;
	int listen_fd = -1;
	int sock = -1;
	char line[128];
	int ret = 1;

	memset(&bo, 0, sizeof(bo));
	bo.drm_fd = -1;
	bo.dmabuf_fd = -1;

	if (init_rdma_context(opt, &r))
		goto out;

	if (create_amdgpu_bo(opt, &bo))
		goto out;

	if (register_dmabuf_mr(r.pd, bo.dmabuf_fd, opt->iova, opt->length,
			       &dmabuf_mr))
		goto out;

	fill_local_info(&r, dmabuf_mr->rkey, opt->iova, opt->length, &local);

	listen_fd = tcp_listen(opt->tcp_port);
	if (listen_fd < 0)
		goto out;

	printf("server listening on tcp port %s\n", opt->tcp_port);
	sock = accept(listen_fd, NULL, NULL);
	if (sock < 0) {
		perror("accept");
		goto out;
	}
	printf("client connected on control socket\n");

	if (exchange_peer_info(sock, &local, &remote))
		goto out;
	if (remote.length != opt->length) {
		fprintf(stderr, "length mismatch: local=%zu remote=%" PRIu64 "\n",
			opt->length, remote.length);
		goto out;
	}

	if (connect_qp(&r, opt, &remote))
		goto out;
	if (control_barrier(sock, "READY"))
		goto out;

	if (read_line(sock, line, sizeof(line)))
		goto out;
	if (strncmp(line, "CLIENT_DONE", 11)) {
		fprintf(stderr, "client did not complete successfully: %s", line);
		goto out;
	}

	if (!opt->skip_mmap_verify) {
		if (mmap_verify_amdgpu_bo(&bo, opt->length, opt->pattern_seed))
			goto out;
	} else {
		printf("server mmap verify skipped by option\n");
	}

	if (write_all(sock, "SERVER_OK\n", strlen("SERVER_OK\n")))
		goto out;

	printf("server p2p target test passed\n");
	ret = 0;

out:
	if (ret && sock >= 0)
		write_all(sock, "SERVER_FAIL\n", strlen("SERVER_FAIL\n"));
	if (dmabuf_mr && ibv_dereg_mr(dmabuf_mr))
		perror("ibv_dereg_mr dmabuf");
	close_amdgpu_bo(&bo);
	if (sock >= 0)
		close(sock);
	if (listen_fd >= 0)
		close(listen_fd);
	cleanup_rdma_context(&r);
	return ret;
}

static int run_client(const struct options *opt)
{
	struct rdma_context r;
	struct ibv_mr *mr = NULL;
	uint8_t *buf = NULL;
	struct peer_info local;
	struct peer_info remote;
	int sock = -1;
	char line[128];
	int ret = 1;

	if (init_rdma_context(opt, &r))
		goto out;

	if (register_normal_mr(r.pd, opt->length, &buf, &mr))
		goto out;
	fill_pattern(buf, opt->length, opt->pattern_seed);

	fill_local_info(&r, mr->rkey, (uintptr_t)buf, opt->length, &local);

	sock = tcp_connect(opt->server_host, opt->tcp_port);
	if (sock < 0)
		goto out;
	printf("connected to server %s tcp port %s\n",
	       opt->server_host, opt->tcp_port);

	if (exchange_peer_info(sock, &local, &remote))
		goto out;
	if (remote.length != opt->length) {
		fprintf(stderr, "length mismatch: local=%zu remote=%" PRIu64 "\n",
			opt->length, remote.length);
		goto out;
	}

	if (connect_qp(&r, opt, &remote))
		goto out;
	if (control_barrier(sock, "READY"))
		goto out;

	if (post_rdma_op(r.qp, IBV_WR_RDMA_WRITE, buf, opt->length, mr->lkey,
			 remote.vaddr, remote.rkey, "ibv_post_send RDMA_WRITE"))
		goto out;
	if (poll_completion(r.cq, "RDMA_WRITE"))
		goto out;

	memset(buf, 0, opt->length);
	if (post_rdma_op(r.qp, IBV_WR_RDMA_READ, buf, opt->length, mr->lkey,
			 remote.vaddr, remote.rkey, "ibv_post_send RDMA_READ"))
		goto out;
	if (poll_completion(r.cq, "RDMA_READ"))
		goto out;

	if (verify_pattern(buf, opt->length, opt->pattern_seed,
			   "client RDMA_READ back"))
		goto out;

	if (write_all(sock, "CLIENT_DONE\n", strlen("CLIENT_DONE\n")))
		goto out;
	if (read_line(sock, line, sizeof(line)))
		goto out;
	if (strcmp(line, "SERVER_OK\n")) {
		fprintf(stderr, "server reported failure: %s", line);
		goto out;
	}

	printf("client p2p initiator test passed\n");
	ret = 0;

out:
	if (ret && sock >= 0)
		write_all(sock, "CLIENT_FAIL\n", strlen("CLIENT_FAIL\n"));
	if (mr && ibv_dereg_mr(mr))
		perror("ibv_dereg_mr normal");
	free(buf);
	if (sock >= 0)
		close(sock);
	cleanup_rdma_context(&r);
	return ret;
}

int main(int argc, char **argv)
{
	struct options opt;

	if (parse_options(argc, argv, &opt)) {
		usage(argv[0]);
		return 2;
	}

	printf("configuration: role=%s device=%s ib_port=%d gid_index=%d length=%zu tcp_port=%s pattern_seed=0x%02x\n",
	       opt.role == ROLE_SERVER ? "server" : "client",
	       opt.device, opt.ib_port, opt.gid_index, opt.length,
	       opt.tcp_port, opt.pattern_seed);

	if (opt.role == ROLE_SERVER) {
		printf("server target: drm_node=%s domain=0x%" PRIx64 " flags=0x%" PRIx64 " iova=0x%" PRIx64 "\n",
		       opt.drm_node, opt.amdgpu_domain, opt.amdgpu_flags,
		       opt.iova);
		return run_server(&opt);
	}

	return run_client(&opt);
}
