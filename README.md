# RDMA dma-buf MR demo

这个 demo 验证链路中的第一段：

```text
AMDGPU BO 或已有 dma-buf fd -> libibverbs/uverbs -> dmabuf MR -> lkey/rkey
```

`BO` 是 `Buffer Object`，在这里指 DRM/AMDGPU 驱动管理的一块 GPU buffer。`VRAM BO` 是请求放在 `AMDGPU_GEM_DOMAIN_VRAM` 里的 AMDGPU buffer object。

当前支持三种模式：

- `--normal`：注册普通用户态内存，用来确认 RDMA verbs 基础环境正常。
- `--dmabuf-fd`：注册当前进程里已经打开的 dma-buf fd。
- `--amdgpu-drm`：通过 AMDGPU DRM UAPI 创建 GEM BO，PRIME export 成 dma-buf fd，再注册成 RDMA dmabuf MR。

`rdma_dmabuf_p2p_test` 是下一阶段对测程序：server 端持有 AMDGPU VRAM dmabuf MR，client 端建立 RC QP 后执行 RDMA Write/Read，并做 pattern 校验。

本机排查中遇到过 `ibv_reg_dmabuf_mr()` 成功但 BO 最终落到 GTT 的情况，现象和分析见 [rdma_dmabuf_p2p_gtt_fallback_analysis.md](rdma_dmabuf_p2p_gtt_fallback_analysis.md)。后续遇到类似问题，按 [rdma_dmabuf_p2p_debug_playbook.md](rdma_dmabuf_p2p_debug_playbook.md) 的步骤建立判断链。

## 1. 依赖

需要用户态 verbs 头文件和库：

```bash
sudo apt-get install -y libibverbs-dev rdma-core
```

当前系统如果只有 `/usr/include/rdma/*`，但没有 `/usr/include/infiniband/verbs.h`，说明缺少 `libibverbs-dev`，这个 demo 不能编译。

## 2. 编译

```bash
make
```

也可以直接运行测试脚本，它会自动编译并执行基础自检：

```bash
./run_tests.sh
```

常用参数：

```bash
./run_tests.sh --device mlx5_0 --length 4096
./run_tests.sh --drm-node /dev/dri/renderD128 --amdgpu-domain vram
./run_tests.sh --skip-amdgpu
```

本机实测时 Mellanox/RoCE 设备名不是 `mlx5_0`，而是 `rocep1s0f0`。先用 `ibv_devices` 或 `ibv_devinfo -l` 看真实 verbs 设备名。

如果当前用户没有 `/dev/dri/renderD*` 权限，但你只是想临时验证 AMDGPU exporter 阶段，可以使用：

```bash
./run_tests.sh --sudo-amdgpu --drm-node /dev/dri/renderD128
```

长期更合适的做法是把当前用户加入 `render` 组后重新登录。

## 3. 普通 MR 自检

先验证普通用户态内存注册是否工作：

```bash
./rdma_dmabuf_mr_demo --device mlx5_0 --normal --length 4096
```

预期看到 `normal MR registered` 和 `lkey/rkey`。

## 4. dma-buf MR 注册

假设你已经有一个 GPU buffer export 出来的 dma-buf fd，例如 fd `7`：

```bash
./rdma_dmabuf_mr_demo --device mlx5_0 --dmabuf-fd 7 --length 4096 --iova 0
```

注意：shell 命令行不能直接跨进程传递“另一个进程打开的 fd 7”。实际使用时，GPU export 和本 demo 的 RDMA 注册要在同一个进程里完成，或者通过 Unix domain socket 用 `SCM_RIGHTS` 传 fd。本 demo 的 `--dmabuf-fd` 适合嵌入或改造成同进程测试。

## 5. AMDGPU exporter 自检

如果机器上有 AMD render node，可以让 demo 在同一个进程里创建 BO、导出 dma-buf，并注册 MR：

```bash
ls -l /dev/dri
./rdma_dmabuf_mr_demo --device mlx5_0 --amdgpu-drm /dev/dri/renderD128 --length 4096 --amdgpu-domain vram
```

`--amdgpu-domain` 可选：

- `vram`：优先创建显存 BO，这是验证 GPU VRAM P2P 的目标路径。
- `gtt`：创建 GTT/system-memory backed BO，适合先验证 exporter 和 dmabuf MR 注册链路。
- `cpu`：AMDGPU CPU domain，通常只适合功能探测，不代表 VRAM P2P。
- 也可以传数值，直接作为 `AMDGPU_GEM_CREATE.in.domains`。

`--amdgpu-flags` 会原样写入 `AMDGPU_GEM_CREATE.in.domain_flags`，默认是 `0`。第一阶段建议不要加特殊 flag，先把基础链路跑通。

如果 `vram` 失败，可以先试 `--amdgpu-domain gtt`。GTT 成功只能说明 dma-buf export/register 能工作，不能证明 RNIC 已经直接访问 GPU VRAM。

## 6. 结果含义

如果成功：

```text
amdgpu BO exported as dma-buf
dmabuf MR registered
lkey=...
rkey=...
```

说明：

- libibverbs 能打开 RDMA device；
- PD 创建成功；
- 当前 libibverbs 中存在 `ibv_reg_dmabuf_mr` 符号；
- AMDGPU DRM 能创建并导出 BO；
- 内核/provider 接受该 dma-buf fd 并创建了 MR。

这还不等于已经证明真实 VRAM P2P。下一步还要做 RDMA Write/Read 数据正确性验证，并确认路径没有退回 system memory/GTT。

## 7. 本机实测记录

2026-06-20 在当前机器上执行：

```bash
./run_tests.sh --device rocep1s0f0 --length 4096 \
  --drm-node /dev/dri/renderD128 --amdgpu-domain vram
```

关键结果：

```text
ibv_devinfo -d rocep1s0f0
  state: PORT_ACTIVE
  link_layer: Ethernet

normal MR registered
addr=... length=4096 lkey=0x2604 rkey=0x2604

amdgpu BO exported as dma-buf
drm_node=/dev/dri/renderD128 gem_handle=1 dmabuf_fd=6 length=4096 domain=0x4 flags=0x0

dmabuf MR registered
fd=6 offset=0 length=4096 iova=0x0 lkey=0x2604 rkey=0x2604

failures=0 warnings=0 skips=0
```

这说明当前机器已经打通这些前置链路：

- `rocep1s0f0` 可用于 verbs MR 注册；
- normal MR 注册成功，基础 RDMA 用户态环境正常；
- `/dev/dri/renderD128` 是 `amdgpu`；
- AMDGPU 可以创建 `domain=0x4` 的 VRAM BO；
- 这个 VRAM BO 可以 PRIME export 成 dma-buf fd；
- `ibv_reg_dmabuf_mr()` 可以把该 dma-buf fd 注册成 RDMA MR；
- MR 创建成功并返回 `lkey/rkey`，访问权限包含本地写、远端读和远端写。

边界也要记清楚：当前程序注册成功后就注销 MR，不建 QP，不交换 `rkey/iova`，也不发 RDMA Write/Read。因此这次结果证明“GPU VRAM BO 可以作为 dma-buf 注册成 RDMA MR”，但还不能单独证明 RNIC 已经对 GPU VRAM 完成真实 P2P 数据访问。

## 8. P2P 对测程序

`rdma_dmabuf_p2p_test` 做这些事：

```text
server:
  AMDGPU VRAM BO
  -> PRIME export dma-buf
  -> ibv_reg_dmabuf_mr(iova=0)
  -> 创建 RC QP
  -> 通过 TCP 交换 qpn/psn/gid/rkey/iova
  -> 等 client RDMA Write/Read
  -> mmap AMDGPU BO 校验 pattern

client:
  注册普通用户态 MR
  -> 填充 pattern
  -> 创建 RC QP
  -> RDMA Write 到 server 的 dmabuf MR
  -> RDMA Read 从 server 的 dmabuf MR 读回
  -> 校验读回 pattern
```

server 端，也就是 GPU/RDMA target 机器：

```bash
sudo ./rdma_dmabuf_p2p_test \
  --server \
  --device rocep1s0f0 \
  --ib-port 1 \
  --gid-index 3 \
  --drm-node /dev/dri/renderD128 \
  --length 4096
```

client 端，也就是发起 RDMA Write/Read 的机器：

```bash
./rdma_dmabuf_p2p_test \
  --client <server_ip> \
  --device <client_rdma_dev> \
  --ib-port 1 \
  --gid-index <client_roce_v2_gid_index> \
  --length 4096
```

如果 server 端当前用户已经加入 `render` 组并重新登录，可以不加 `sudo`。否则打开 `/dev/dri/renderD128` 会失败。

默认 server 端创建 `AMDGPU_GEM_DOMAIN_VRAM` BO，并带 `AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED`，这样测试结束后可以 mmap BO 做 CPU 侧 pattern 校验。可以用 `--amdgpu-flags 0` 改回无额外 flag 的创建方式，但某些离散显卡的 VRAM BO 可能不能 CPU mmap，server 端校验会失败；这时可以临时加 `--skip-mmap-verify`，但那只能证明 RDMA Read 回读一致，不能证明 CPU/GPU 可见内容校验也通过。

2026-06-20 本机双进程自回环实测通过：

```text
server:
dmabuf MR registered: fd=6 iova=0x0 length=4096 lkey=0x2605 rkey=0x2605
server listening on tcp port 18516
client connected on control socket
qp connected: local_qpn=137 remote_qpn=138 remote_rkey=0x2706 remote_vaddr=0xaaaad1d51000
server amdgpu BO mmap pattern verify passed, length=4096 seed=0x5a
server p2p target test passed

client:
qp connected: local_qpn=138 remote_qpn=137 remote_rkey=0x2605 remote_vaddr=0x0
RDMA_WRITE completion passed
RDMA_READ completion passed
client RDMA_READ back pattern verify passed, length=4096 seed=0x5a
client p2p initiator test passed
```

这比前面的 MR smoke test 多证明了：QP 建链、远端 `rkey/iova` 交换、RDMA Write、RDMA Read、client 回读校验、server 端 BO mmap 校验都能通过。若要证明跨机器 RoCE 数据面，需要把 client 放到另一台同网段 RoCE 机器上跑同一条 client 命令。

## 9. 常见失败

- `missing /usr/include/infiniband/verbs.h`：安装 `libibverbs-dev`。
- `ibv_reg_dmabuf_mr symbol not found`：当前 rdma-core/libibverbs 太旧，或发行版没有暴露该 API。
- `ibv_get_device_list failed`：RDMA 用户态环境不完整。
- `RDMA device not found`：`--device` 名字错误，先跑 `ibv_devices`。
- `open amdgpu drm node failed`：render node 路径错误，或当前用户无权限访问 `/dev/dri/renderD*`。
- `DRM node driver is 'radeon', expected 'amdgpu'`：当前节点不是 `amdgpu` 驱动，不能使用本 demo 的 AMDGPU GEM_CREATE exporter 路径。
- `DRM_IOCTL_AMDGPU_GEM_CREATE failed`：不是 AMDGPU render node、domain 不支持、显存不足，或权限/驱动状态不满足。
- `DRM_IOCTL_PRIME_HANDLE_TO_FD failed`：BO 不能 PRIME export，或 DRM 节点不支持该导出路径。
- `ibv_reg_dmabuf_mr failed`：dma-buf fd 无效、长度/offset 不合法、GPU exporter 不支持 attach/map、provider/IOMMU/topology 不支持。
- `RDMA_WRITE` 或 `RDMA_READ` completion failed：优先检查两端 GID index、RoCE IP 可达性、MTU、PFC/ECN、rkey/iova 交换是否一致。
- server 端 `mmap amdgpu BO` 失败：VRAM BO 可能不在 CPU visible 区域，先用默认 `AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED`，或临时加 `--skip-mmap-verify` 缩小问题。

## 10. 后续扩展

后续要把 MR 注册扩展成完整 RDMA 数据面：

```text
amdgpu/DRM create BO
  -> export dma-buf fd
  -> register_dmabuf_mr()
  -> exchange rkey/iova
  -> RDMA Write/Read
  -> GPU side verify pattern
```

这样才能避免 fd 跨进程传递问题，并能正确处理 GPU/RDMA 同步。
