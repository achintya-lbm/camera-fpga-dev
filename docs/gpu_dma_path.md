# The GPU DMA receive path (GPUDirect RDMA) and how it is validated

How a sensor frame gets from the DA322 into GPU memory without the host CPU touching pixel data, what
each component does, and which measurements in this repo prove it works. Source references are to
hololink 2.5.0-PB6 (`tools/workspace/hololink`), the same code we build; the FPGA side is the Hololink
IP inside Tauro's bitstream, described in the HSB user guide (`docs/user_guide/dataplane.mdx` in the
holoscan-sensor-bridge repo).

## 1. Path

```
IMX676 ─CSI-2─► DA322 FPGA: D-PHY RX → 64-bit AXI-Stream → Hololink IP packetizer → 10G MAC
                                                                  │  RoCEv2 (UDP/IP) packets, each an RDMA WRITE
                                                                  ▼
ConnectX NIC ── DMA write straight to GPU BAR1 ──► GPU memory: frame page N  (pixels, 4 KB-aligned)
             ── last packet: RDMA WRITE with IMMEDIATE ──► metadata record at the end of page N
                        └── completion queue event ──► RoceReceiver thread (host) ──► RoceReceiverOp emits
                                                        a device tensor that *wraps* page N (zero copy)
                                                        ──► CsiToBayerOp / demosaic / encoder on the GPU
```

Per frame the CPU moves 128 bytes (the metadata record, device→host) and handles one interrupt.
Everything else is NIC→GPU DMA.

## 2. Setup: what the host does once per data channel

`RoceReceiverOp::start_receiver` (`operators/roce_receiver/roce_receiver_op.cpp`) and
`RoceReceiver::start` (`roce_receiver.cpp`):

1. **GPU frame memory.** `ReceiverMemoryDescriptor` calls `cuMemAlloc` for
   `PAGES × (round_up(frame_size, 128) + 128)` bytes, page-aligned; `PAGES = 2`. Each *page* is one
   frame buffer followed by a 128-byte metadata slot. For `FULL_RAW12` (18,946,368 CSI bytes) that is
   2 × 18.9 MB of device memory. On a discrete GPU this is always device memory; the pinned-host
   variant exists only for integrated GPUs.
2. **Export for DMA.** `cuMemGetHandleForAddressRange(..., CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD)` turns
   the allocation into a Linux DMA-BUF file descriptor (needs the open NVIDIA kernel modules).
   `ibv_reg_dmabuf_mr(pd, 0, size, 0, fd, LOCAL_WRITE | REMOTE_WRITE)` hands that buffer to the NIC,
   which pins it and creates a memory region with an `rkey`. If the export fails, hololink falls back
   to `ibv_reg_mr_iova` on the device pointer, which needs `nvidia-peermem`; on our test machine the
   DMA-BUF path is the one in use (the receiver process holds one `dmabuf` fd, no peermem module loaded).
3. **Queue pair.** One completion channel + completion queue, one **UC (unreliable connected)** queue
   pair with `REMOTE_WRITE` access, moved to INIT then RTR. The FPGA never receives anything, so no
   send side is needed; 256 receive work requests are posted so that write-with-immediate completions
   have somewhere to land. The path MTU is set to 4096 and the remote GID is synthesised from the
   board's IPv4 address (RoCEv2, `::ffff:192.168.0.2` form). UC means no acknowledgements and no
   retransmission: a lost packet is a hole in the frame, detected later by the CRC check.
4. **Tell the FPGA where to write.** `DataChannel::configure_roce` (`core/data_channel.cpp`) programs
   the data-plane registers of the sensor's virtual port: destination QP number and `rkey`, the two
   page base addresses (as 128-byte units), the buffer mask (which pages may be used), the frame length
   (`DP_BUFFER_LENGTH`), host MAC/IP/UDP port, and the payload size per packet:
   `((MTU − 74) / 128) × 128` = **1408 bytes** at MTU 1500 (74 = Ethernet+IP+UDP+BTH+RETH+iCRC
   overhead). The packetizer is then enabled.

## 3. Data flow: what happens per frame

- The Hololink IP counts sensor bytes and cuts them into packets. Each packet is a **RoCE RDMA WRITE**
  (BTH opcode 0x2A): RETH virtual address = page base + running offset, `rkey`, DMA length 1408;
  the PSN increments per packet. The NIC validates the rkey and writes the payload directly to the GPU
  address. For a `FULL_RAW12` frame that is 13,457 packets; at 32.6 fps about 440 k packets/s and
  4.9 Gbit/s, which is what the link measurements show.
- When the configured window is full or the sensor's `tlast` arrives, the IP sends one **RDMA WRITE
  with IMMEDIATE** (opcode 0x2B) carrying the 128-byte **metadata record** into the page's metadata
  slot: flags, PSN, frame CRC, PTP timestamp of the frame, `bytes_written`, frame number, PTP timestamp
  of the metadata packet. The 32-bit immediate is `PSN << 8 | page`.
- The immediate consumes a posted receive WR and produces a completion. `RoceReceiver::blocking_monitor`
  sleeps in `poll()` on the completion channel, pops the completion, decodes page and PSN from the
  immediate, starts an async 128-byte `cuMemcpyDtoH` of the metadata slot, records the arrival time,
  bumps `received_frame_number`, and signals `frame_ready`; if the previous frame had not been consumed
  yet it increments `dropped`. It re-posts one receive WR.
- `BaseReceiverOp::compute` (`operators/base_receiver_op.cpp`) picks up the frame: it creates a GXF
  `uint8` tensor of `frame_size` (or `bytes_written` when `trim` is set) that **wraps the page's device
  pointer** — no copy — and copies the metadata record into Holoscan metadata keys
  (`frame_number`, `bytes_written`, `crc`, `psn`, `timestamp_s/ns`, `metadata_s/ns`, `received_s/ns`,
  `received_frame_number`, `dropped`, `imm_data`, `frame_memory`). Downstream operators
  (`CsiToBayerOp`, our `FrameStatsOp`/`FrameCheckOp`, demosaic, JPEG encoder) read the page in GPU memory.
- Double buffering: the FPGA alternates between the two pages (buffer mask `0b11`). The host must
  finish with page N before the FPGA writes it again two frames later; hololink detects a rewritten page
  by comparing the metadata PSN with the PSN from the immediate (`Metadata psn=… but received_psn=…`).
  Our `CameraRig` sizes the downstream pools so the receiver is never the bottleneck (DESIGN §4.5).

## 4. Where the IOMMU comes in

GPUDirect RDMA requires that the NIC's DMA addresses for the GPU buffer are the GPU's BAR1 physical
addresses (NVIDIA GPUDirect RDMA guide, "Supported Systems": all devices must see the same physical
addresses). With Intel VT-d in its default translating mode the NIC sits in its own IOMMU domain
without mappings for the BAR window, so every 4 KB write faults (`DMAR: [DMA Write NO_PASID] Request
device [<nic>] fault addr <inside BAR1>`) and is dropped, while the NIC still signals completions:
frames arrive on time with all-zero contents and zero metadata. `iommu=pt` gives host-owned devices an
identity mapping and keeps VT-d for interrupt remapping/VFIO. Details and the verification commands
are in `docs/bringup/host_setup.md` §2b; the kernel log of the failure is quoted in DESIGN §4.5.

## 5. How this repo validates the path

| Check | Where | What it proves |
|---|---|---|
| **Frame CRC** on every frame: the host copies the received page to pinned memory and computes JAMCRC (`~crc32`) over `bytes_written` bytes; it must equal the FPGA's `crc` metadata | `hsb/ops/frame_check_op.cc`, `bandwidth_test --crc-every 1` | every byte the FPGA sent landed in GPU memory, complete and unmodified: the DMA writes hit the right addresses and nothing was lost on the UC link |
| **Size**: `bytes_written == frame_size` (short frames counted) | `FrameStatsOp`, `FrameCheckOp` | the window/`tlast` framing matches the CSI layout the sensor driver computed |
| **Continuity**: FPGA `frame_number` gaps and receiver `dropped` | `FrameStatsOp` | no frame skipped by the sensor→FPGA path and none overwritten before the host consumed it |
| **Rate**: measured Gbit/s within 3 % of `frame_size × fps` after a 3 s warm-up | `apps/bandwidth_test` | sustained throughput, not just a few good frames |
| **Flat-frame detector**: sampled 32-bit words of a CRC-checked frame take ≤ 4 distinct values | `FrameCheckOp` | catches valid-looking but content-less frames (found when the sensor's line time was pushed too far) |
| **Path evidence**: `ls -l /proc/<pid>/fd | grep dmabuf` = 1, `lsmod | grep peermem` empty, `/sys/bus/pci/devices/<nic>/iommu_group/type` = `identity`, `journalctl -k | grep DMAR` silent during the run | `docs/bringup/host_setup.md` §2b | the frames really travelled NIC→GPU by DMA-BUF, not through host memory |
| **Content**: dumped frames decoded by `tools/py/analysis/raw_frame.py`, live preview, JPEG XS lossless round trips on captured frames | `docs/hardware/imx676_samples.md`, `compression/docs/compression_study.md` | pixel data is the sensor's image, not scrambled or offset |
| **Negative control**: same stack with the IOMMU translating | WORKING.md 2026-09-21 | completions at frame rate, all-zero pages, DMAR faults — the failure mode is recognisable |
| **Reference path**: Linux UDP receiver (one `recv()` per packet into pinned host memory, one `cuMemcpyHtoDAsync` per frame) gives identical CRCs and rates | `docs/bandwidth.md` 2026-09-21 rows | the GPUDirect path adds nothing the socket path does not also see |

Results (`docs/bandwidth.md`, 2026-09-22, CAM4 over RoCE, CRC on every frame, 30 s each):

| Mode | fps | Rate | Frames | Gaps / drops | CRC |
|---|---|---|---|---|---|
| `FULL_RAW10` | 29.98 | 3.787 Gbit/s | 897 | 0 / 0 | 897/897 |
| `FULL_RAW12` | 32.57 | 4.937 Gbit/s | 975 | 0 / 0 | 975/975 |
| `CROP_1280X720_RAW10` | 149.27 | 1.376 Gbit/s | 4473 | 0 / 0 | 4473/4473 |
| `BIN2_RAW12_60` (2026-09-23) | 59.96 | 2.273 Gbit/s | 1796 | 0 / 0 | 1796/1796, 0 flat |

Two cameras (J1C + J1D, 7.6 Gbit/s aggregate) ran the same way on 2026-09-23 with zero drops.

## 6. Limits and things to know

- **UC transport**: no retransmission. Packet loss corrupts a frame; the CRC check is the only guard.
  The 10G link is point-to-point, so loss has not been observed.
- **Two pages, fixed.** A slow consumer does not stall the FPGA; it overwrites the oldest page and the
  host counts `dropped` / logs a PSN mismatch. Back-pressure exists only inside the Holoscan graph.
- **MTU 1500 → 1408-byte payloads** (efficiency 1408/1502 ≈ 94 %). The FPGA build option `HOST_MTU 4096`
  would raise it; the host side follows the enumerated MTU.
- **Latency metadata** needs the FPGA's PTP clock synchronised to the host (ptp4l); until then
  `FrameStatsOp` prints `latency=n/a`.
- **CRC checking costs a device→host copy per checked frame** (25 MB at full resolution). It is a test
  instrument (`--crc-every`), not part of the production pipeline.
- **Ownership of the page**: the tensor wraps the receiver's buffer with a no-op deleter. Operators must
  not hold a frame tensor across more than one frame time, or the FPGA will overwrite it underneath them.
