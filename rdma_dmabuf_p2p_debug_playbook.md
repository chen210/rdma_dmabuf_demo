# RDMA dma-buf P2P 排查手册

本文整理 `rdma_dmabuf_p2p_test` 的排查方法。目标是建立判断链，而不是只记录某一次输出：

```text
程序请求了什么
  -> AMDGPU 实际创建/export 了什么 dma-buf
  -> RNIC 是否 attach 这个 dma-buf
  -> amdgpu/TTM 最终把 BO 放在 VRAM 还是 GTT
  -> RDMA Write/Read 通过后到底证明了什么
```

核心原则：

- `domain=0x4` 只表示用户态请求 `AMDGPU_GEM_DOMAIN_VRAM`，不是最终 placement。
- `ibv_reg_dmabuf_mr()` 成功只证明 MR 注册前置条件成立，不等于已经证明 RNIC 直写 VRAM。
- RDMA Write/Read 和 pattern 校验成功后，还要确认 BO placement 是 `VRAM VISIBLE`，否则可能只是 RDMA 写到了 GTT/system RAM。
- debugfs 中 `VRAM_CLEARED`、`VRAM_CONTIGUOUS` 是 flag，不是 placement。placement 看 size 后面的 `VRAM VISIBLE` 或 `GTT`。

## 1. 先确认程序请求

server 常用命令：

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
server target: drm_node=/dev/dri/renderD128 domain=0x4 flags=0x1 iova=0x0
amdgpu BO exported: drm_node=/dev/dri/renderD128 gem_handle=1 dmabuf_fd=6 length=1048576 domain=0x4 flags=0x1
dmabuf MR registered: fd=6 iova=0x0 length=1048576 lkey=... rkey=...
```

解释：

- `domain=0x4`：程序向 AMDGPU 请求 `AMDGPU_GEM_DOMAIN_VRAM`。
- `flags=0x1`：默认带 `AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED`，方便最后 mmap 做 CPU 侧校验。
- `dmabuf_fd=6`：server 进程里 dma-buf fd 号，后面要用它关联内核对象。
- `length=1048576`：用户请求的 BO/MR 长度。AMDGPU/TTM 内部可能按页块向上取整，所以 debugfs 中可能看到 `2097152 byte`。

这里能确认“请求目标是 VRAM”，不能确认“最终一定在 VRAM”。

## 2. 确认 server 进程和 fd

如果 server 仍在 `server listening on tcp port 18516`，先找到宿主机上的真实进程：

```bash
ps -eo pid,ppid,stat,cmd | grep rdma_dmabuf_p2p_test
```

典型结果：

```text
1624 ... sudo ./rdma_dmabuf_p2p_test --server ...
1626 ... sudo ./rdma_dmabuf_p2p_test --server ...
1627 ... ./rdma_dmabuf_p2p_test --server ...
```

真正持有 fd 的通常是最后那个实际程序进程，例如 `1627`。

查看 fd：

```bash
sudo ls -l /proc/1627/fd
```

典型结果：

```text
3 -> /dev/infiniband/uverbs0
5 -> /dev/dri/renderD128
6 -> /dmabuf:
7 -> socket:[...]
```

这说明：

- fd 3 是 verbs device。
- fd 5 是 AMDGPU render node。
- fd 6 是程序输出里的 dma-buf。
- fd 7 是 control socket。

继续读 fdinfo：

```bash
sudo cat /proc/1627/fdinfo/6
```

典型结果：

```text
ino:    2
size:   1048576
count:  4
exp_name: drm
```

这里拿到两个关键字段：

- `ino=2`：dma-buf inode，用来和 `/sys/kernel/debug/dma_buf/bufinfo`、`amdgpu_vm_info` 中的 `exported as ino:N` 关联。
- `size=1048576`：这个 dma-buf 对外暴露的大小。

## 3. 确认 RNIC 是否 attach 了 dma-buf

读取 dma-buf debugfs：

```bash
sudo cat /sys/kernel/debug/dma_buf/bufinfo
```

典型结果：

```text
Dma-buf Objects:
size    	flags   	mode    	count   	exp_name	ino     	name
01048576	00000002	02080007	00000004	drm	00000002	<none>
	kernel fence:0:0   signalled
	Attached Devices:
	0000:01:00.0
Total 1 devices attached
```

判断方法：

- 找 `ino` 是否等于 fdinfo 中的 `ino`。
- 找 `size` 是否等于 fdinfo 中的 `size`。
- 看 `Attached Devices` 是否有 RNIC 的 PCI BDF。

如果能看到 RNIC BDF，例如 `0000:01:00.0`，说明 mlx5/RDMA importer 已 attach 这个 dma-buf。它证明 attach/map 链路发生了，但仍然不证明 BO 在 VRAM。

## 4. 判断最终 placement

最关键的是 AMDGPU debugfs：

```bash
sudo grep -n -E '1048576|2097152|VRAM|GTT|pin count|exported|ino:2' \
  /sys/kernel/debug/dri/0/amdgpu_vm_info
```

如果看到：

```text
1048576 byte VRAM VISIBLE exported as ino:2 ...
```

或：

```text
2097152 byte VRAM VISIBLE pin count 1 ...
```

说明这个 BO 当前在 visible VRAM。结合 RDMA Write/Read 校验通过，才更接近证明 RNIC 访问 GPU VRAM。

如果看到：

```text
1048576 byte GTT exported as ino:2 ...
```

或：

```text
2097152 byte GTT pin count 1 CPU_ACCESS_REQUIRED ...
```

说明最终 placement 是 GTT。此时 RDMA 数据面即使通过，也只能说明：

```text
RNIC DMA -> system RAM pages
GPU      -> 通过 GART/GTT 访问这些 system RAM pages
```

不能说已经证明：

```text
RNIC DMA -> GPU PCIe BAR / visible VRAM -> GPU VRAM
```

注意两个常见陷阱：

- `VRAM_CONTIGUOUS` 不等于 placement 在 VRAM，它是 flag。
- 用户请求 1 MiB 时，debugfs 中可能显示为 2 MiB；需要结合 fdinfo、pin count、exported inode、运行前后变化一起判断。

## 5. 用 sysfs accounting 做旁证

server listening 时读取：

```bash
GPU=/sys/class/drm/renderD128/device
cat $GPU/mem_info_vram_used
cat $GPU/mem_info_vis_vram_used
cat $GPU/mem_info_gtt_used
```

判断方法：

- 如果新 BO 创建后 `mem_info_vram_used` / `mem_info_vis_vram_used` 增长，支持 VRAM placement 判断。
- 如果主要是 `mem_info_gtt_used` 变化，支持 GTT fallback 判断。

sysfs accounting 是旁证，不如 `amdgpu_vm_info` 的 per-BO placement 直接。

## 6. 排查顺序

推荐按这个顺序走：

```text
1. verbs 设备是否可用
   ibv_devices
   ibv_devinfo -d rocep1s0f0

2. DRM 节点是否是 amdgpu
   程序会检查 /dev/dri/renderD128 的 driver name

3. BO 是否创建并 export
   看 "amdgpu BO exported"

4. dmabuf MR 是否注册
   看 "dmabuf MR registered"

5. client/server 是否完成 RDMA Write/Read
   看 RDMA_WRITE completion、RDMA_READ completion、pattern verify

6. fd 是否对应当前 dma-buf
   /proc/<pid>/fd
   /proc/<pid>/fdinfo/<dmabuf_fd>

7. RNIC 是否 attach
   /sys/kernel/debug/dma_buf/bufinfo

8. BO 最终 placement
   /sys/kernel/debug/dri/0/amdgpu_vm_info

9. 如果是 GTT，再查 topology/IOMMU
   lspci -tv
   readlink -f /sys/class/infiniband/<dev>/device
   readlink -f /sys/class/drm/renderD128/device
   cat /proc/cmdline
   dmesg | grep -i iommu
```

这个顺序的好处是每一步都只回答一个问题。不要在 `ibv_reg_dmabuf_mr()` 成功后直接跳到“P2P 已经 OK”的结论。

## 7. GTT fallback 后看什么

如果最终是 GTT，优先检查两类原因。

第一类是 PCIe topology：

```bash
readlink -f /sys/class/infiniband/rocep1s0f0/device
readlink -f /sys/class/drm/renderD128/device
sudo lspci -tv
```

更理想的拓扑是：

```text
Root Port
  -> PCIe Switch
       -> GPU
       -> RNIC
```

如果 GPU 和 RNIC 挂在不同 root port 后面，Linux `pci_p2pdma_distance()` 很可能不认为它们适合 P2P，AMDGPU dma-buf map 阶段就会退到 GTT。

第二类是 IOMMU：

```bash
cat /proc/cmdline
dmesg | grep -i iommu
```

如果当前是 translated/strict IOMMU domain，可以做 A/B 测试：

```text
iommu.passthrough=1
```

或平台支持时测试：

```text
iommu=pt
```

重启后用同一条 server/client 命令复测，再看 `amdgpu_vm_info` 是否从 `GTT` 变成 `VRAM VISIBLE`。

## 8. 结论模板

写测试结论时建议用这种格式：

```text
本次已经证明：
- AMDGPU BO 可以创建并 export 成 dma-buf。
- mlx5/RDMA 可以 attach 并注册 dmabuf MR。
- RC QP + RDMA Write/Read + pattern verify 可以闭环。

本次尚未证明：
- RNIC 直接写入 GPU VRAM。

原因：
- 当前 server BO 最终 placement 是 GTT，而不是 VRAM VISIBLE。
- 因此当前数据路径更准确地描述为 RDMA 到 GTT/system RAM。
```

如果 `amdgpu_vm_info` 显示 `VRAM VISIBLE exported` 或明确的 `VRAM VISIBLE pin count 1`，结论才可以升级为：

```text
本次在当前 topology/IOMMU 配置下，server BO 保持在 visible VRAM，
并且 RDMA Write/Read 数据面校验通过，因此可以证明 RNIC 对 GPU visible VRAM 的 P2P 路径成立。
```

## 9. 2026-06-20 VRAM 复测记录

这次复测的目标是确认 `--amdgpu-domain vram` 后，server 侧 P2P 目标到底是 VRAM 还是 GTT。

运行时 sysfs accounting 曾观察到：

```text
mem_info_vram_used:     83439616 -> 84758528
mem_info_vis_vram_used: 83439616 -> 84758528
mem_info_gtt_used:      41701376 -> 41701376
```

这个变化支持“新对象落在 visible VRAM”，但它只是全局计数，不能单独证明某个 dma-buf 的 placement。

随后读取 server 进程 fd 和 dma-buf debugfs：

```text
/proc/5269/fdinfo/6:
ino: 14172

/sys/kernel/debug/dma_buf/bufinfo:
size      count  exp_name  ino  attached
01048576  4      drm       4    0000:01:00.0
08355840  2      drm       1    <none>
```

这里能确认：

- 当前有一个 1 MiB dma-buf 对象，debugfs inode 是 `4`。
- 这个 1 MiB dma-buf 已经被 `0000:01:00.0` attach，说明 RNIC importer attach 发生过。
- 另一个 8355840 byte dma-buf 是 `ino:1`，没有 attached device，不是这次 RDMA target。
- 在这台内核上，不要直接把 `/proc/<pid>/fdinfo/<fd>` 里的 `ino:14172` 当成 dma-buf debugfs 里的 `ino:4` 使用；当前输出中两者没有直接相等。

同一轮 `amdgpu_vm_info` 的过滤输出中有这些关键行：

```text
2097152 byte GTT pin count 1 CPU_ACCESS_REQUIRED VRAM_CONTIGUOUS
2097152 byte VRAM VISIBLE VRAM_CLEARED
2097152 byte GTT pin count 1 CPU_ACCESS_REQUIRED VRAM_CONTIGUOUS
8355840 byte VRAM VISIBLE pin count 1 exported as ino:1 NO_CPU_ACCESS VRAM_CLEARED VRAM_CONTIGUOUS
2097152 byte GTT pin count 1 CPU_ACCESS_REQUIRED VRAM_CONTIGUOUS
```

这次过滤结果的关键问题是：没有出现 `exported as ino:4`。因此它还不能把 1 MiB、RNIC attached 的目标 dma-buf 和某一条 `VRAM VISIBLE` 或 `GTT` placement 行直接关联起来。

当前可以得出的结论：

```text
已经证明：
- 1 MiB DRM dma-buf 存在。
- RNIC BDF 0000:01:00.0 attach 了这个 1 MiB dma-buf。
- sysfs accounting 显示 visible VRAM 用量增长，GTT 用量不变，支持 VRAM placement。

尚未从这份 amdgpu_vm_info 过滤输出中直接证明：
- debugfs inode 4 对应的 1 MiB dma-buf placement 是 VRAM VISIBLE。
```

下一步应保留更多上下文，而不是只 grep 少数关键词：

```bash
sudo nl -ba /sys/kernel/debug/dri/0/amdgpu_vm_info | sed -n '1,170p'
sudo grep -n -C 3 -E 'exported as ino:4|01048576|1048576|00000004|2097152 byte (VRAM VISIBLE|GTT).*pin count 1' \
  /sys/kernel/debug/dri/0/amdgpu_vm_info
```

如果后续能看到同一对象明确为：

```text
... VRAM VISIBLE ... exported as ino:4 ...
```

或能通过完整上下文把 `ino:4` 所在对象和 `VRAM VISIBLE pin count 1` 关联起来，结论才可以升级为“这次 RDMA P2P target 是 visible VRAM”。如果关联到的是 `GTT pin count 1`，则仍应按 GTT fallback 处理。

补充 grep：

```bash
sudo grep -n -C 5 -E 'exported as ino:4|ino:4|01048576|1048576|2097152 byte (VRAM VISIBLE|GTT).*pin count 1' \
  /sys/kernel/debug/dri/0/amdgpu_vm_info
```

输出没有命中 `exported as ino:4` 或 `ino:4`，但命中了三个 `pin count 1` 的 2 MiB BO：

```text
10:  2097152 byte GTT pin count 1 CPU_ACCESS_REQUIRED VRAM_CONTIGUOUS
31:  2097152 byte GTT pin count 1 CPU_ACCESS_REQUIRED VRAM_CONTIGUOUS
150: 2097152 byte GTT pin count 1 CPU_ACCESS_REQUIRED VRAM_CONTIGUOUS
```

同时附近有一些 `VRAM VISIBLE` 2 MiB BO，但它们没有 `pin count 1`，也没有 `exported as ino:4`：

```text
28:  2097152 byte VRAM VISIBLE VRAM_CLEARED
146: 2097152 byte VRAM VISIBLE CPU_ACCESS_REQUIRED VRAM_CLEARED
148: 2097152 byte VRAM VISIBLE NO_CPU_ACCESS VRAM_CLEARED
```

这条补充证据使当前判断更偏向 GTT fallback：已被 RDMA attach/map 后仍处于 pin 状态的 2 MiB BO，在这份输出里显示为 `GTT pin count 1`，而不是 `VRAM VISIBLE pin count 1`。

## 10. 用 ftrace / bpftrace 做更明确判断

`rdma_dmabuf_p2p_test` 现在会给 server 侧导出的 dma-buf 设置 debug name。默认格式类似：

```text
rdma-p2p-<pid>-<gem_handle>
```

也可以手动指定：

```bash
sudo ./rdma_dmabuf_p2p_test \
  --server \
  --device rocep1s0f0 \
  --ib-port 1 \
  --gid-index 3 \
  --drm-node /dev/dri/renderD128 \
  --length 0x100000 \
  --amdgpu-domain vram \
  --dmabuf-name rdma-p2p-vram-test
```

程序会打印：

```text
dmabuf debug name set: pid=<pid> fd=<fd> name=<name>
dmabuf debug inspect: sudo cat /proc/<pid>/fdinfo/<fd>; sudo cat /sys/kernel/debug/dma_buf/bufinfo
```

注意：`/proc/<pid>/fdinfo/<fd>` 里的普通 `ino` 不是 dma-buf debugfs 里用于 `exported as ino:N` 的 inode。更可靠的关联方式是：

```text
程序输出的 pid/fd/name
  -> /proc/<pid>/fdinfo/<fd> 中的 size/count/exp_name/name
  -> /sys/kernel/debug/dma_buf/bufinfo 中同名对象的 size/ino/Attached Devices
  -> amdgpu_vm_info 中同一个 "exported as ino:N" 的 placement
```

### 10.1 ftrace：确认 dma-buf inode 和 RNIC attach

先开 ftrace：

```bash
sudo ./tools/trace_dma_buf_ftrace.sh
```

再在另一个终端运行 server/client。期望看到：

```text
dma_buf_fd:             exp_name=drm size=1048576 ino=<N> fd=<fd>
dma_buf_dynamic_attach: exp_name=drm size=1048576 ino=<N> ... dev_name=0000:01:00.0
```

这一步能证明：

- 当前进程 fd 对应的 dma-buf debugfs inode 是 `<N>`。
- RNIC 设备 `0000:01:00.0` attach 了同一个 dma-buf。

它还不能单独证明 VRAM/GTT，因为 dma_buf tracepoint 不包含 AMDGPU TTM placement。

### 10.2 bpftrace：观察 AMDGPU map 走了哪个 placement 分支

如果内核/module BTF 可用，可以直接跑：

```bash
sudo bpftrace ./tools/trace_amdgpu_dmabuf_map.bt
```

再运行同一组 server/client。脚本会在 `amdgpu_dma_buf_map()` 返回时打印：

```text
amdgpu_dma_buf_map ret=... ino=<N> size=1048576 dev=0000:01:00.0 peer2peer=1 branch=<B> placement=<P>
```

判断：

```text
branch=3 placement=VRAM -> TTM_PL_VRAM -> VRAM
branch=2 placement=GTT  -> TTM_PL_TT   -> GTT/system memory
```

这个脚本不读取 `struct drm_gem_object` 或 `struct amdgpu_bo`，因为一些发行版内核没有暴露这些 DRM/AMDGPU 私有类型的 BTF。它改为观察 `amdgpu_dma_buf_map()` 内部实际调用的分支函数：

```text
drm_prime_pages_to_sg()      -> GTT path
amdgpu_vram_mgr_alloc_sgt()  -> VRAM path
```

这比 `mem_info_vram_used` 更明确，因为它是在 AMDGPU dma-buf map 路径里判断实际走了 VRAM 分支还是 GTT 分支。

2026-06-20 实测：直接读取 `struct drm_gem_object` / `struct amdgpu_bo` 的 bpftrace 版本会报：

```text
Cannot resolve unknown type "struct drm_gem_object"
Cannot resolve unknown type "struct amdgpu_bo"
```

因此当前仓库保留的是分支跟踪版脚本。用当前 bpftrace v0.23.2 执行：

```bash
sudo bpftrace --dry-run ./tools/trace_amdgpu_dmabuf_map.bt
```

已通过 dry-run，输出：

```text
Attaching 6 probes...
```

如果 bpftrace 报 BTF/struct 解析失败，则退回两种办法：

1. 继续用 ftrace + `dma_buf/bufinfo` + `amdgpu_vm_info` 做 inode 关联。
2. 临时在内核 `drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c:amdgpu_dma_buf_map()` 里加 `pr_info()`，打印 `file_inode(dma_buf->file)->i_ino`、`attach->dev`、`attach->peer2peer` 和 `bo->tbo.resource->mem_type`。

### 10.3 bpftrace 实测 VRAM 成功案例

2026-06-20 用 4 KiB BO 复测：

```bash
sudo bpftrace ./tools/trace_amdgpu_dmabuf_map.bt

sudo ./rdma_dmabuf_p2p_test \
  --server \
  --device rocep1s0f0 \
  --ib-port 1 \
  --gid-index 3 \
  --drm-node /dev/dri/renderD128 \
  --length 4096 \
  --amdgpu-domain vram \
  --dmabuf-name rdma-p2p-vram-test

./rdma_dmabuf_p2p_test \
  --client 127.0.0.1 \
  --device rocep1s0f0 \
  --ib-port 1 \
  --gid-index 3 \
  --length 4096
```

bpftrace 抓到：

```text
amdgpu_dma_buf_map ret=0xffff002094852650 ino=8 size=4096 dev=0000:01:00.0 peer2peer=1 branch=3 placement=VRAM
```

server 侧关键输出：

```text
dmabuf debug name set: pid=11677 fd=6 name=rdma-p2p-vram-test
amdgpu BO exported: drm_node=/dev/dri/renderD128 gem_handle=1 dmabuf_fd=6 length=4096 domain=0x4 flags=0x1
dmabuf MR registered: fd=6 iova=0x0 length=4096 lkey=0x2707 rkey=0x2707
server amdgpu BO mmap pattern verify passed, length=4096 seed=0x5a
server p2p target test passed
```

client 侧关键输出：

```text
RDMA_WRITE completion passed
RDMA_READ completion passed
client RDMA_READ back pattern verify passed, length=4096 seed=0x5a
client p2p initiator test passed
```

这次可以明确写成：

```text
对于本次 4 KiB 测试，AMDGPU dma-buf map 路径走 branch=3 / VRAM，
RNIC BDF 为 0000:01:00.0，peer2peer=1，
并且 RC QP RDMA Write/Read + pattern verify 通过。
因此本次证明了 RNIC 对 AMDGPU VRAM dma-buf MR 的 P2P 数据面闭环。
```

边界：这个结论只覆盖本次 `length=4096` 的对象。更大 BO，例如 1 MiB，仍应独立用同一个 bpftrace 方法复测，因为 TTM placement 可能受大小、可见 VRAM 空间、flags、拓扑和 IOMMU 状态影响。
