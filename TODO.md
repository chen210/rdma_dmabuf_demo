# TODO

This document tracks follow-up work for the RDMA dma-buf demo after the basic
MR registration and P2P correctness tests.

## Performance Validation

- Add throughput tests for RDMA Write and RDMA Read against dma-buf MRs.
- Measure latency for small transfers, including 4 KiB and cache-line-sized
  operations if the hardware path supports meaningful results.
- Test several buffer sizes:
  - 4 KiB
  - 64 KiB
  - 1 MiB
  - 16 MiB or larger, depending on available VRAM
- Compare these paths:
  - normal userspace MR
  - AMDGPU GTT-backed dma-buf MR
  - AMDGPU VRAM-backed dma-buf MR
- Record whether the AMDGPU map path is `placement=VRAM` or `placement=GTT`
  for every performance run.
- Keep the benchmark output machine-readable enough for later plotting.

## CPU Performance and System Impact

- Track CPU usage on both initiator and target during throughput tests.
- Capture per-core utilization, not only total CPU usage.
- Watch for unexpected CPU copies or CPU polling overhead.
- Record interrupt rate, context switches, and softirq activity during large
  transfers.
- Compare CPU cost between:
  - normal MR transfers
  - GTT dma-buf transfers
  - VRAM dma-buf transfers
- Add a repeatable command sequence using tools such as `perf stat`, `mpstat`,
  `pidstat`, or `/proc/stat`.

## VRAM Data Processing Features

- Add a new test mode that treats the VRAM buffer as real data, not only a byte
  pattern target.
- Start with simple image-like buffers:
  - grayscale image buffer
  - RGBA image buffer
  - tiled or row-stride layout
- Let the RDMA client write image data into the server-side AMDGPU VRAM
  dma-buf.
- Add a server-side verification step that interprets the VRAM contents as an
  image or structured data block.
- Explore a GPU-side processing path after RDMA writes complete:
  - checksum or histogram
  - color conversion
  - simple filter such as invert, threshold, or blur
  - data transform for non-image buffers
- Add a way to export the processed result for validation:
  - RDMA Read back to the client
  - CPU mmap verification when the BO is CPU-visible
  - optional dump to a raw image file for inspection

## Correctness Boundaries

- Keep performance results separate from correctness claims.
- For every VRAM-related result, record:
  - buffer size
  - AMDGPU domain request
  - AMDGPU flags
  - bpftrace placement result
  - PCIe topology
  - IOMMU mode
  - RDMA device and GID index
- Do not treat `--amdgpu-domain vram` as proof of final VRAM placement.
- Treat each buffer size as a separate placement result until proven otherwise.

## Documentation

- Add a benchmark section to `README.md` once throughput and latency tests are
  implemented.
- Keep detailed raw investigation notes in separate debug documents.
- Summarize stable, repeatable commands in the README.

