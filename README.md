# RDMA dma-buf MR demo

这个 demo 验证链路中的第一段：

```text
已有 dma-buf fd -> libibverbs/uverbs -> dmabuf MR -> lkey/rkey
```

它不负责创建 AMD GPU 显存，也不负责把 ROCm/HIP allocation export 成 dma-buf。GPU buffer export 应该由单独的 DRM/ROCm/amdgpu 代码完成；本 demo 只接收一个已经存在的 dma-buf fd。

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

## 5. 结果含义

如果成功：

```text
dmabuf MR registered
lkey=...
rkey=...
```

说明：

- libibverbs 能打开 RDMA device；
- PD 创建成功；
- 当前 libibverbs 中存在 `ibv_reg_dmabuf_mr` 符号；
- 内核/provider 接受该 dma-buf fd 并创建了 MR。

这还不等于已经证明真实 VRAM P2P。下一步还要做 RDMA Write/Read 数据正确性验证，并确认路径没有退回 system memory/GTT。

## 6. 常见失败

- `missing /usr/include/infiniband/verbs.h`：安装 `libibverbs-dev`。
- `ibv_reg_dmabuf_mr symbol not found`：当前 rdma-core/libibverbs 太旧，或发行版没有暴露该 API。
- `ibv_get_device_list failed`：RDMA 用户态环境不完整。
- `RDMA device not found`：`--device` 名字错误，先跑 `ibv_devices`。
- `ibv_reg_dmabuf_mr failed`：dma-buf fd 无效、长度/offset 不合法、GPU exporter 不支持 attach/map、provider/IOMMU/topology 不支持。

## 7. 后续扩展

后续可以把 AMD GPU export 逻辑接到同一个进程里：

```text
amdgpu/DRM create BO
  -> export dma-buf fd
  -> register_dmabuf_mr()
  -> exchange rkey/iova
  -> RDMA Write/Read
  -> GPU side verify pattern
```

这样才能避免 fd 跨进程传递问题，并能正确处理 GPU/RDMA 同步。
