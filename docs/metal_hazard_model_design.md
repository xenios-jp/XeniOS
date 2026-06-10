# Metal backend-owned hazard model — design and rollout plan

Referenced by the `metal_backend_hazard_model` / `metal_backend_hazard_model_validate`
cvars (`metal_command_processor.cc`).

## Status

- **Phase 1 (shared-memory buffer) is implemented.** `metal_backend_hazard_model`
  creates the buffer `HazardTrackingModeUntracked` with ordering through the
  fence edges below; `metal_backend_hazard_model_validate` emits the same
  edges while driver tracking stays on (soak mode, no behavior change).
  Edge accounting is reported in the telemetry dump (`hazard_model` line).
- **Phase 2 (EDRAM buffer) is implemented** behind
  `metal_backend_hazard_model_edram`: the buffer is created untracked and
  every EDRAM-touching encoder (RT dump, host depth store, resolve copy,
  init fill, snapshot restore) waits on `XeniaEdramFence` at creation and
  updates it at end. Readers update too — a fence wait only orders against
  the last update, so the read-side update is what orders a later writer
  after an in-flight reader (WAR). The init fill updates without waiting and
  commits before any other EDRAM work, so every later wait has a committed
  prior update. Caveat: the `edram_load_vs/ps` draw path (fragment-stage
  EDRAM reads) is currently dead code; if revived it needs a render-stage
  wait/update pair.
- **Phase 3 (render targets) is implemented** behind
  `metal_backend_hazard_model_render_targets`: the RT heap pool is untracked
  and `XeniaRenderTargetFence` linearizes RT access with fragment-stage
  scoping (render encoders wait before Fragment / update after Fragment, so
  vertex/binning of pass N+1 still overlaps fragment of pass N; EDRAM dump,
  host depth store, direct host resolve, RT transfer blits and RT texture
  uploads wait at creation / update at end). RTs are destroyed only at cache
  clear, so RT heap reuse cannot alias mid-session. Trace-dump readback paths
  drain the queue and are not separately fenced.
- **Phase 4 (texture-cache heaps) is implemented** behind
  `metal_backend_hazard_model_texture_heaps`: the texture heap pool is
  untracked; texture-writing encoders (untile compute, upload blits,
  deferred-batch compute/blit) update `XeniaTextureUploadFence` at end and
  render encoders wait at creation before Vertex|Object|Mesh|Fragment.
  Standalone upload command buffers commit before the main command buffer
  (the MTLFence cross-CB requirement). Texture eviction is gated on completed
  submissions (`TextureCache::CompletedSubmissionUpdated`), so heap reuse
  cannot alias in-flight reads. The CPU (non-blit) upload path writes
  textures after `waitUntilCompleted` and needs no GPU edge.
- **useHeap suppression**: a heap that is untracked AND covered by the queue
  residency set skips the per-encoder `useHeap` (counted as
  `use_heap covered` in telemetry). Per Apple, hazard tracking only covers
  directly-bound resources within one command queue, so an untracked heap's
  `useHeap` carried residency only — which `MTLResidencySet` already
  provides. Tracked heaps (phase off, or validate mode) keep the call.
- **Cross-queue presenter reads** of the swap texture
  (`MetalPresenter::CopyTextureToGuestOutput`, separate `MTLCommandQueue`)
  were never ordered by hazard tracking (tracking is per-queue) and continue
  to rely on the existing commit/submission handshake; the phases neither
  depend on nor change this.
- **Residency sets are implemented** (`metal_residency_sets`, auto-detected,
  on when supported): shared memory, EDRAM, the bindless and native-MSL
  argument heaps, and texture pool heaps are queue-resident, and covered
  resources skip the per-encoder useResource/useHeap re-apply. This matches
  the Metal 4 direction (Game Porting Toolkit 4 skills: useResource/useHeap
  are removed in Metal 4 in favor of MTLResidencySet).
- **Rollout per title**: run with `metal_backend_hazard_model_validate=true`
  first (all phases' edges emitted, tracking kept, zero behavior change),
  soak ~60s of gameplay, confirm no visual diffs and read the `hazard_model`
  telemetry line; then enable the per-phase cvars one at a time
  (`metal_backend_hazard_model`, `_edram`, `_texture_heaps`,
  `_render_targets`) with a visual A/B each. A missed edge manifests as
  flicker/corruption, not a crash.

Per the GPTK 4 `managing-metal4-synchronization` skill: the producer stage is
the resource's previous usage and the consumer stage its new usage; `MTLFence`
orders encoders on the same queue **across command buffers** (commit order
applies), so the standalone-command-buffer edges are fence-coverable;
`MTLEvent` is only needed for cross-queue work.

Implemented phase-1 fence edges (producer -> fence -> consumer):

| Edge | Producer | Consumer |
|---|---|---|
| Upload blit -> draws | `EndSharedMemoryUploadBlitEncoder` updateFence when copies were encoded | render encoder creation waits before Vertex/Object/Mesh |
| Upload blit -> untile compute | same | texture cache compute encoder creation waits |
| Upload blit -> DMA index copy | encoder split when the open upload encoder has writes | `EncodeSharedMemoryBlitReadDependency` waits unconditionally under the model |
| Memexport render writes -> readers | existing `UpdateSharedMemoryFenceForActiveRenderEncoder` | waits above |
| Direct host resolve compute -> readers | resolve encoder updateFence | waits above |
| CPU direct writes | none needed: ordered by command buffer commit boundaries | — |

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
