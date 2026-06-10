# Metal backend-owned hazard model — design and rollout plan

Referenced by the `metal_backend_hazard_model` / `metal_backend_hazard_model_validate`
cvars (`metal_command_processor.cc`). As of this writing the cvars are defined
but unconsumed; this document is the implementation checklist for wiring them.

## What is already done (do not redo)

The "useResource reduction" half of the original goal is implemented:

- Heap-backed texture-cache allocations are made resident via `useHeap` on the
  containing heap, not per-texture `useResource`
  (`BuildBindlessTextureResourceSet`, metal_command_processor.cc).
- Residency sets are applied per encoder with serial-based dedup
  (`ApplyRenderEncoderResourceSet`: `applied_serial == set.serial` skip), and
  `useResource` calls are batched 128 at a time with usage/stage grouping.
- An open-addressing per-encoder dedup table avoids redundant `useResource`
  for ad-hoc resources (`render_encoder_resource_usage_table_`).

The remaining win is dropping **driver hazard tracking** on the hot buffers so
encoder creation stops paying for dependency analysis, plus (later, iOS 18+)
`MTLResidencySet` to drop the per-encoder residency re-apply entirely.

## Current synchronization inventory

Resources currently `HazardTrackingModeTracked` (driver-ordered):

| Resource | Where created | Tracked today |
|---|---|---|
| Shared memory buffer (512 MB) | `metal_shared_memory.cc` (`newBuffer`, default tracking) | yes |
| Texture heap pool heaps | `metal_heap_pool.cc:110` (explicit Tracked) | yes |
| EDRAM buffer | render target cache | yes |
| Upload pool buffers | `metal_upload_buffer_pool.cc` | yes |

Existing explicit fence (`shared_memory_fence_`) edges — already correct and
redundant with tracking:

- Render encoders that wrote shared memory (memexport) call
  `updateFence` at encoder end (`UpdateSharedMemoryFenceForActiveRenderEncoder`).
- Compute writers (direct host resolve into shared memory) update the fence.
- Readers with a known pending-write overlap wait:
  `EncodeSharedMemoryRenderReadDependencies` (render),
  `EncodeSharedMemoryBlitReadDependency` (blit).

## Phase 1: untracked shared-memory buffer (behind the cvar)

Goal: create the shared-memory buffer untracked when
`metal_backend_hazard_model` is set; all ordering through explicit fences.

Complete consumer inventory of `MetalSharedMemory::GetBuffer()` (every edge
must be fenced before flipping the default):

| Consumer | File:line (at time of writing) | Encoder type | Direction | Fence edge needed |
|---|---|---|---|---|
| Vertex/index fetch binds | metal_command_processor.cc:6654-6753, 8905 | render | read | wait at encoder begin (vertex stages) after any upload/write fence |
| Memexport UAV bind | metal_command_processor.cc:5584 | render | write | update at encoder end (exists) |
| Guest DMA index copy | metal_command_processor.cc:6364 | blit (shared upload encoder) | read | `EncodeSharedMemoryBlitReadDependency` (exists) |
| Upload blits (`UploadRanges`) | metal_shared_memory.cc | blit | write | **add** `updateFence` in `EndSharedMemoryUploadBlitEncoder` |
| Texture untile source | metal_texture_cache.cc:1818 | compute | read | **add** `waitForFence` when the upload compute encoder is created |
| Direct host resolve destination | metal_direct_host_resolve.cc:567 | compute | write | update (exists via compute-write path) |
| Trace dump readback | metal_command_processor.cc:1427/1844 | blit | read | wait (exists or full drain) |

Hard problems that fences do NOT solve (must be handled differently):

1. **Standalone command buffers.** The texture cache can batch uploads into a
   standalone command buffer committed before the main one
   (`TrackStandaloneGpuAccess`). `MTLFence` does not order across command
   buffers; ordering today comes from hazard tracking + serial queue
   scheduling. Either route all shared-memory-reading work into the main
   command buffer when the hazard model is on, or use `MTLEvent` for these
   edges.
2. **CPU direct writes** (`metal_shared_memory_direct_write`): CPU writes to
   the shared-storage buffer are ordered by command buffer commit boundaries,
   not fences. Unchanged by this work, but the validator must not flag them.

Rollout: implement the two missing edges + the standalone-CB answer, run with
`metal_backend_hazard_model_validate` (tracker shadow-logging) across the
regression titles, then flip per-title, then default.

## Phase 2+: EDRAM buffer, texture heaps, residency sets

- EDRAM buffer: the RT cache already brackets every transfer/dump/resolve with
  `shared_memory_fence_`-style edges; inventory as above before untracking.
- Texture heaps: untracking the pool heap removes false cross-texture
  dependencies (upload of texture A currently serializes draws sampling
  texture B from the same heap) but requires fencing upload-vs-sample per
  texture generation; needs the validate mode first.
- `MTLResidencySet` (iOS 18+): attach the long-lived heaps/buffers to the
  command queue's residency set and stop calling `useHeap`/`useResource` for
  them per encoder. Gate on availability; keep the current path as fallback.

## Why this was not blind-implemented in one pass

Every missed fence edge is silent GPU data corruption that only reproduces on
device. The shadow validator (`metal_backend_hazard_model_validate`) exists in
name precisely so each phase can be proven per title before it changes
behavior; the inventory above is the checklist for that implementation.
