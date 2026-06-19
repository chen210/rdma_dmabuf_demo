# RDMA dma-buf MR demo

这个 demo 验证链路中的第一段：

```text
AMDGPU BO 或已有 dma-buf fd -> libibverbs/uverbs -> dmabuf MR -> lkey/rkey
```

当前支持三种模式：

- `--normal`：注册普通用户态内存，用来确认 RDMA verbs 基础环境正常。
- `--dmabuf-fd`：注册当前进程里已经打开的 dma-buf fd。
- `--amdgpu-drm`：通过 AMDGPU DRM UAPI 创建 GEM BO，PRIME export 成 dma-buf fd，再注册成 RDMA dmabuf MR。

它还不是完整 RDMA 数据面程序：不会建 QP，不会和远端交换 `rkey/iova`，也不会验证 GPU buffer 里的 pattern。它只验证“export/register MR”这一步。

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

## 7. 常见失败

- `missing /usr/include/infiniband/verbs.h`：安装 `libibverbs-dev`。
- `ibv_reg_dmabuf_mr symbol not found`：当前 rdma-core/libibverbs 太旧，或发行版没有暴露该 API。
- `ibv_get_device_list failed`：RDMA 用户态环境不完整。
- `RDMA device not found`：`--device` 名字错误，先跑 `ibv_devices`。
- `open amdgpu drm node failed`：render node 路径错误，或当前用户无权限访问 `/dev/dri/renderD*`。
- `DRM node driver is 'radeon', expected 'amdgpu'`：当前节点不是 `amdgpu` 驱动，不能使用本 demo 的 AMDGPU GEM_CREATE exporter 路径。
- `DRM_IOCTL_AMDGPU_GEM_CREATE failed`：不是 AMDGPU render node、domain 不支持、显存不足，或权限/驱动状态不满足。
- `DRM_IOCTL_PRIME_HANDLE_TO_FD failed`：BO 不能 PRIME export，或 DRM 节点不支持该导出路径。
- `ibv_reg_dmabuf_mr failed`：dma-buf fd 无效、长度/offset 不合法、GPU exporter 不支持 attach/map、provider/IOMMU/topology 不支持。

## 8. 后续扩展

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
