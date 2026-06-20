# RDMA dma-buf Demo

This repository contains small, focused test programs for validating RDMA
registration and data movement with Linux dma-buf objects, especially dma-bufs
exported from AMDGPU buffer objects.

The main purpose is to separate three questions that are often mixed together:

1. Can the RDMA stack register a normal userspace MR?
2. Can an AMDGPU BO be exported as a dma-buf and registered with
   `ibv_reg_dmabuf_mr()`?
3. Does the real RDMA data path write/read the target GPU buffer, and is that
   buffer actually placed in VRAM rather than GTT/system memory?

## Contents

- `rdma_dmabuf_mr_demo.c`
  - Smoke test for normal MR registration.
  - Registers an existing dma-buf fd.
  - Can create an AMDGPU BO, export it as a dma-buf, and register it as an RDMA
    dma-buf MR.

- `rdma_dmabuf_p2p_test.c`
  - End-to-end RC QP test.
  - Server owns an AMDGPU dma-buf MR.
  - Client performs RDMA Write and RDMA Read.
  - Both sides verify a deterministic byte pattern.

- `tools/trace_dma_buf_ftrace.sh`
  - Uses dma-buf tracepoints to identify the dma-buf inode and the importing
    device.

- `tools/trace_amdgpu_dmabuf_map.bt`
  - Uses bpftrace to determine whether AMDGPU dma-buf mapping went through the
    GTT path or the VRAM path.

- `run_tests.sh`
  - Builds the binaries and runs basic smoke tests.

- `README.zh-CN.md`
  - Original Chinese usage notes.

- `rdma_dmabuf_p2p_debug_playbook.md`
  - Debug playbook for proving or disproving real VRAM P2P.

- `rdma_dmabuf_p2p_gtt_fallback_analysis.md`
  - Notes from a GTT fallback investigation.

## Requirements

Install the RDMA userspace headers and tools:

```bash
sudo apt-get install -y libibverbs-dev rdma-core
```

For AMDGPU tests, the machine must have an AMDGPU render node such as:

```text
/dev/dri/renderD128
```

For placement tracing, `bpftrace` and root privileges are required.

## Build

```bash
make
```

This builds:

```text
rdma_dmabuf_mr_demo
rdma_dmabuf_p2p_test
```

## Quick Smoke Test

First verify basic RDMA verbs support with a normal MR:

```bash
./rdma_dmabuf_mr_demo --device mlx5_0 --normal --length 4096
```

On some RoCE systems the verbs device is not named `mlx5_0`. Check the real
device name with:

```bash
ibv_devices
ibv_devinfo -l
```

## AMDGPU dma-buf MR Registration

Create an AMDGPU BO, export it as a dma-buf, and register it as an RDMA MR:

```bash
./rdma_dmabuf_mr_demo \
  --device mlx5_0 \
  --amdgpu-drm /dev/dri/renderD128 \
  --length 4096 \
  --amdgpu-domain vram
```

This proves that the AMDGPU exporter path and RDMA dma-buf MR registration path
work. It does not, by itself, prove that RDMA wrote to GPU VRAM.

## End-to-End P2P Test

Run the server on the GPU/RDMA target machine:

```bash
sudo ./rdma_dmabuf_p2p_test \
  --server \
  --device mlx5_0 \
  --ib-port 1 \
  --gid-index 3 \
  --drm-node /dev/dri/renderD128 \
  --length 4096 \
  --amdgpu-domain vram
```

Run the client from the RDMA initiator:

```bash
./rdma_dmabuf_p2p_test \
  --client <server_ip> \
  --device <client_rdma_dev> \
  --ib-port 1 \
  --gid-index <client_gid_index> \
  --length 4096
```

Expected successful output includes:

```text
RDMA_WRITE completion passed
RDMA_READ completion passed
client RDMA_READ back pattern verify passed
server amdgpu BO mmap pattern verify passed
```

## Proving VRAM Instead of GTT

`--amdgpu-domain vram` only means the userspace program requested VRAM. It does
not guarantee the final AMDGPU/TTM placement.

To observe the actual map path:

```bash
sudo bpftrace ./tools/trace_amdgpu_dmabuf_map.bt
```

Then run the P2P server/client test. The trace script reports:

```text
branch=3 placement=VRAM
```

or:

```text
branch=2 placement=GTT
```

Only `placement=VRAM`, combined with successful RDMA Write/Read and pattern
verification, proves the VRAM P2P path for that specific test size and system
configuration.

## Notes

- GTT success is still useful: it proves dma-buf export, attach, and RDMA MR
  registration are working.
- VRAM placement may depend on BO size, CPU visibility flags, IOMMU mode, PCIe
  topology, and AMDGPU/TTM decisions.
- Larger BO sizes should be tested independently. A 4 KiB VRAM result does not
  automatically prove a 1 MiB BO will also remain in VRAM.

