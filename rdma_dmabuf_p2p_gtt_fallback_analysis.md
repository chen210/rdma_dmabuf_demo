# RDMA dma-buf P2P GTT fallback analysis

本文记录 2026-06-20 在当前机器上用 `rdma_dmabuf_p2p_test` 验证 AMDGPU dma-buf MR 时遇到的现象、排查思路和结论。

如果要按步骤复用这套排查方法，见 [rdma_dmabuf_p2p_debug_playbook.md](rdma_dmabuf_p2p_debug_playbook.md)。本文偏向记录本机这次 GTT fallback 案例。

核心结论：

```text
ibv_reg_dmabuf_mr() 成功
RC QP + RDMA Write/Read 数据面可以闭环
但本次 1 MiB server BO 最终 placement 是 GTT，不是 VRAM
因此这次不能证明 RNIC 直接 P2P 写入 GPU VRAM
```

## 1. 测试目标

目标不是只看 `ibv_reg_dmabuf_mr()` 是否成功，而是进一步确认：

```text
AMDGPU BO
  -> PRIME export dma-buf
  -> mlx5/RDMA 注册 dmabuf MR
  -> RNIC RDMA Write/Read
  -> BO 内容校验
  -> BO 最终仍在 VRAM VISIBLE
```

其中最后一项非常关键。`domain=0x4` 只表示用户态向 AMDGPU GEM_CREATE 请求 `AMDGPU_GEM_DOMAIN_VRAM`，不能证明 TTM 最终把 BO 放在 VRAM。

## 2. Server 端现象

server 端执行：

```bash
sudo ./rdma_dmabuf_p2p_test \
  --server \
  --device rocep1s0f0 \
  --ib-port 1 \
  --gid-index 3 \
  --drm-node /dev/dri/renderD128 \
  --length 0x100000
```

关键输出：

```text
configuration: role=server device=rocep1s0f0 ib_port=1 gid_index=3 length=1048576 tcp_port=18516 pattern_seed=0x5a
server target: drm_node=/dev/dri/renderD128 domain=0x4 flags=0x1 iova=0x0
rdma context ready: device=rocep1s0f0 port=1 gid_index=3 qpn=138 psn=15313717 lid=0
amdgpu BO exported: drm_node=/dev/dri/renderD128 gem_handle=1 dmabuf_fd=6 length=1048576 domain=0x4 flags=0x1
dmabuf MR registered: fd=6 iova=0x0 length=1048576 lkey=0x2708 rkey=0x2708
server listening on tcp port 18516
```

这说明这些前置条件已经成立：

- RDMA device `rocep1s0f0` 可打开。
- PD/CQ/QP 等 RDMA 基础对象可创建。
- `/dev/dri/renderD128` 是可用的 AMDGPU render node。
- AMDGPU 能创建 1 MiB BO，并 PRIME export 为 dma-buf fd。
- mlx5 provider 接受该 dma-buf fd，并成功创建 dmabuf MR。

但这仍然没有说明 BO 最终在 VRAM。

## 3. sysfs 计数现象

server 保持在 `listening` 状态时，读取 GPU memory accounting：

```bash
GPU=/sys/class/drm/renderD128/device
cat $GPU/mem_info_vram_used
cat $GPU/mem_info_gtt_used
cat $GPU/mem_info_vis_vram_used
```

多次观测结果类似：

```text
83709952
39604224
83709952

83709952
42749952
83709952

83709952
40652800
83709952
```

现象：

- `mem_info_vram_used` 没有随 1 MiB BO 增长。
- `mem_info_vis_vram_used` 也没有增长。
- `mem_info_gtt_used` 有变化。

这已经提示 BO 很可能没有常驻 VRAM，而是走到了 GTT/system RAM backing pages。

## 4. debugfs per-BO 证据

进一步读取 AMDGPU debugfs：

```bash
sudo grep -n -E '1048576|VRAM|GTT|pin count' /sys/kernel/debug/dri/0/amdgpu_vm_info
sudo cat /sys/kernel/debug/dri/0/amdgpu_vram_mm
sudo cat /sys/kernel/debug/dri/0/amdgpu_gtt_mm
```

关键行：

```text
9:        0x00000000:      1048576 byte GTT exported as ino:7 VRAM_CLEARED  kernel fence:159:24   signalled
17:       Total invalidated size:      1048576 objs: 1
```

这行是本次 1 MiB exported BO 的直接证据：

```text
size      = 1048576
state     = exported
placement = GTT
```

`VRAM_CLEARED` 只是 BO 创建时内核追加的清零 flag，不表示 BO 位于 VRAM。真正 placement 要看 `1048576 byte` 后面的 `GTT` / `VRAM VISIBLE` 字段。

`amdgpu_vram_mm` 也显示：

```text
size: 1073741824
usage: 83709952
vis usage:83709952
```

这和 sysfs 计数一致：本次 1 MiB BO 没有计入 VRAM/visible VRAM。

## 5. VRAM、GTT 和 RAM 的边界

AMDGPU UAPI 中常见 domain：

```text
AMDGPU_GEM_DOMAIN_CPU  = 0x1
AMDGPU_GEM_DOMAIN_GTT  = 0x2
AMDGPU_GEM_DOMAIN_VRAM = 0x4
```

含义：

- `VRAM`：GPU 本地显存。离散 GPU 会通过 PCIe BAR aperture 暴露可见部分。
- `GTT`：system RAM pages，通过 GPU GART/GTT 映射进 GPU 地址空间。
- `CPU`：普通系统内存 domain，不代表 GPU VRAM P2P。

如果 BO 在 VRAM，理想 P2P 路径是：

```text
RNIC DMA -> GPU PCIe BAR / visible VRAM aperture -> GPU VRAM
```

如果 BO 在 GTT，实际路径变成：

```text
RNIC DMA -> system RAM pages
GPU      -> 通过 GART/GTT 访问这些 system RAM pages
```

第二条路径可以让 RDMA Write/Read 和 BO 内容校验成功，但它不是 RNIC 直写 GPU VRAM。

## 6. 为什么请求 VRAM 仍会落到 GTT

`rdma_dmabuf_p2p_test` server 端默认请求：

```text
domain = AMDGPU_GEM_DOMAIN_VRAM
flags  = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED
```

也就是输出中的：

```text
domain=0x4 flags=0x1
```

但是 AMDGPU/TTM 中，用户态请求 VRAM 不等于最终只能放 VRAM。内核中有两个关键点。

第一，用户态 BO 请求纯 VRAM 时，非 kernel BO 的 `allowed_domains` 会扩展为 `VRAM|GTT`，这样驱动在显存压力、迁移或 attach/map 时可以退到 GTT。

第二，AMDGPU dma-buf map 路径默认从 GTT 开始，只有 attachment 被认为可以 peer-to-peer 时才把 VRAM 加入候选：

```c
unsigned int domains = AMDGPU_GEM_DOMAIN_GTT;

if (bo->preferred_domains & AMDGPU_GEM_DOMAIN_VRAM &&
    attach->peer2peer) {
    bo->flags |= AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
    domains |= AMDGPU_GEM_DOMAIN_VRAM;
}
amdgpu_bo_placement_from_domain(bo, domains);
```

所以 `attach->peer2peer` 是关键条件。mlx5 dmabuf importer 侧声明了：

```c
static struct dma_buf_attach_ops mlx5_ib_dmabuf_attach_ops = {
    .allow_peer2peer = 1,
    .invalidate_mappings = mlx5_ib_dmabuf_invalidate_cb,
};
```

但 AMDGPU attach 阶段还会检查 PCIe P2P distance。如果平台拓扑或 PCIe 路径不满足 P2P，AMDGPU 会把 `attach->peer2peer` 清掉，后续 map 就会退到 GTT。

## 7. PCIe 拓扑证据

本机设备路径：

```bash
readlink -f /sys/class/infiniband/rocep1s0f0/device
readlink -f /sys/class/drm/renderD128/device
```

输出：

```text
/sys/devices/pci0000:00/0000:00:00.0/0000:01:00.0
/sys/devices/pci0000:00/0000:00:12.0/0000:08:00.0
```

`lspci -tv` 相关部分：

```text
-+-[0000:00]-+-00.0-[01]--+-00.0  Mellanox Technologies MT27710 Family [ConnectX-4 Lx]
 |           |            \-00.1  Mellanox Technologies MT27710 Family [ConnectX-4 Lx]
 |           ...
 |           \-12.0-[08]--+-00.0  Advanced Micro Devices, Inc. [AMD/ATI] Oland [Radeon HD 8570 / R5 430 OEM / R7 240/340 / Radeon 520 OEM]
 |                        \-00.1  Advanced Micro Devices, Inc. [AMD/ATI] Oland/Hainan/Cape Verde/Pitcairn HDMI Audio [Radeon HD 7000 Series]
```

也就是说：

```text
RNIC: 0000:00:00.0 root port -> 0000:01:00.0 ConnectX-4 Lx
GPU : 0000:00:12.0 root port -> 0000:08:00.0 AMD Oland
```

它们在同一个 PCI domain 下，但挂在不同 root port 后面，不是在同一个下游 PCIe switch 下。Linux `pci_p2pdma_distance()` 对这种跨 root port 路径通常会判定为不适合 P2P。当前 BO placement 为 `GTT exported`，正好吻合这个判断。

本机还有另一个 RDMA 设备：

```text
0000:7c -> 0000:7d Huawei HNS GE/10GE/25GE RDMA Network Controller
```

它在另一个 PCI segment/root complex 下，离当前 GPU 更远，通常更不适合验证这块 GPU 的 VRAM P2P。

## 8. IOMMU 影响

当前启动参数没有显式 passthrough：

```text
BOOT_IMAGE=/boot/vmlinuz-7.1.0-rc5-chenxy-00029-g5dece85a7987-dirty root=... ro ... pcie_aspm=off quiet
```

`dmesg` 中 IOMMU 信息：

```text
iommu: Default domain type: Translated
iommu: DMA domain TLB invalidation policy: strict mode
```

这说明当前 IOMMU 是 translated domain，而且是 strict 模式。IOMMU 配置确实可能影响 PCIe P2P，所以值得做 A/B 测试：

```text
iommu.passthrough=1
```

或在平台支持时测试：

```text
iommu=pt
```

重启后确认：

```bash
cat /proc/cmdline
dmesg | grep -i iommu
```

然后重新跑同样的 server 测试，再看：

```bash
sudo grep -n -E '1048576|VRAM|GTT|pin count|exported' /sys/kernel/debug/dri/0/amdgpu_vm_info
```

如果从：

```text
1048576 byte GTT exported ...
```

变成：

```text
1048576 byte VRAM VISIBLE exported ...
```

说明 IOMMU 配置是关键因素之一。

如果仍然是 `GTT exported`，主要问题就不是 verbs 或 demo 参数，而是 PCIe topology/root port P2P 不通过。

## 9. PCIe BAR 与 visible VRAM

本机 GPU resource 显示：

```text
BAR0: 0x0000080040000000 - 0x000008007fffffff
```

大小为 1 GiB。

同时 sysfs 显示：

```text
mem_info_vram_total     = 1073741824
mem_info_vis_vram_total = 1073741824
```

所以这张 Oland 卡当前是 1 GiB VRAM，并且 1 GiB 都是 visible VRAM。对 1 MiB 这类测试，BAR/visible VRAM 容量本身不是瓶颈。

真正的问题是 RDMA dma-buf attach/map 阶段没有被判定为可 P2P，所以 BO 被放到了 GTT。

## 10. 后续验证建议

### 10.1 当前平台继续验证

当前平台可以继续验证这些问题：

```bash
# 1. IOMMU passthrough A/B
cat /proc/cmdline
dmesg | grep -i iommu

# 2. 重新跑 1 MiB server 测试
sudo ./rdma_dmabuf_p2p_test \
  --server \
  --device rocep1s0f0 \
  --ib-port 1 \
  --gid-index 3 \
  --drm-node /dev/dri/renderD128 \
  --length 0x100000

# 3. 在 server listening 时看 per-BO placement
sudo grep -n -E '1048576|VRAM|GTT|pin count|exported' /sys/kernel/debug/dri/0/amdgpu_vm_info
```

成功证明 VRAM P2P 的目标输出应类似：

```text
1048576 byte VRAM VISIBLE exported ...
```

如果仍然是：

```text
1048576 byte GTT exported ...
```

说明还没有证明 RNIC 直写 VRAM。

### 10.2 换 PCIe 拓扑验证

更理想的硬件拓扑：

```text
Root Port
  -> PCIe Switch
       -> GPU
       -> RNIC
```

也就是 GPU 和 RNIC 在同一个 PCIe switch 的下游端口下，并且 ACS/IOMMU/平台固件没有强制把 peer traffic 重定向回 root complex。

换拓扑后重新看：

```bash
sudo lspci -tv
readlink -f /sys/class/infiniband/<rdma_dev>/device
readlink -f /sys/class/drm/renderD128/device
sudo grep -n -E '1048576|VRAM|GTT|pin count|exported' /sys/kernel/debug/dri/0/amdgpu_vm_info
```

## 11. 本次测试边界

本次已经证明：

- AMDGPU BO 可以创建并 export 成 dma-buf。
- `ibv_reg_dmabuf_mr()` 可以注册这个 dma-buf fd。
- `rdma_dmabuf_p2p_test` 可以建立 RC QP 并做 RDMA Write/Read 数据面校验。
- debugfs 可以看到对应 1 MiB BO 的最终 placement。

本次没有证明：

- RNIC 直接 P2P 写入 GPU VRAM。
- 当前 PCIe 拓扑支持 GPU/RNIC VRAM P2P。
- IOMMU passthrough 后一定能让该拓扑通过 P2P。

当前最准确的描述是：

```text
当前机器上 dmabuf MR 注册和 RDMA 数据面可通，
但 server 端 1 MiB AMDGPU exported BO 最终为 GTT placement，
因此这次路径是 RDMA 到 GTT/system RAM，不是 RDMA 到 VRAM。
```
