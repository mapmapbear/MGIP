# GPU-Driven Rendering Engineering Plan

**Date:** 2026-05-08  
**Project:** VKDemo  
**Author:** Codex

---

## Overview

This document converts the existing GPU-Driven design into an engineering delivery plan based on the current repository state.

This plan now distinguishes three different targets that should not be conflated:

1. **Object-level GPU-Driven backend**
   This is the current delivery target and the only scope considered executable in this plan. It means:
   - a dedicated GPU-driven pass graph
   - persistent object registration and GPU culling inputs
   - object-level indirect draw submission for the main opaque path
   - incremental scene upload and scene switching stability

2. **Complete GPU-Driven renderer**
   This is a broader future target. It includes the object-level backend, but additionally requires:
   - a GPU-driven lighting/shading path
   - a functionally complete Hi-Z / visibility / compaction chain
   - same-frame GPU-owned visibility consumption instead of CPU-side feedback loops
   - removal of remaining hybrid dependencies on shared legacy subsystems

3. **Meshlet-driven renderer**
   This is a separate future target beyond the current shipping path. It requires:
   - production meshlet generation
   - production meshlet visibility/culling
   - meshlet-native draw submission
   - a deliberate decision to make meshlets part of the shipping renderer instead of an experimental branch

The **object-level GPU-Driven backend** is complete enough to serve as the active base. Current execution scope now includes:

- completion and validation of the object-level shipping path
- closure work for the **complete GPU-driven renderer** on top of that path

The **meshlet-driven renderer** remains documented below for planning purposes, but is still **out of execution scope**.

The current implementation already contains these foundation pieces:

- `GPUDrivenRenderer` backend switch via `RendererFacade`
- persistent scene object registration via `GPUSceneRegistry`
- shared scene vertex/index buffer path
- GPU culling input buffer integration
- MDI-capable GBuffer / Depth / CSM shadow paths
- async loading coordinator and batch upload helpers

The current implementation does **not** yet provide a complete GPU-Driven renderer. The main gaps are:

- GPU-Driven passes are still placeholders
- main rendering still depends on legacy `Renderer`
- scene rebuild is full rebuild instead of incremental update
- Hi-Z, batch building, sorting, and meshlet culling are not functionally complete
- meshlet and GPU scene shaders are still stubs

This plan defines the work needed to move from the current hybrid state to an engineering-ready **object-level GPU-Driven backend**, while also documenting the future work required for a complete GPU-driven renderer and a meshlet-driven renderer.

---

## Completion Criteria

### Active Completion Criteria: Object-Level GPU-Driven Backend

The current execution target is considered complete only when all of the following are true:

- `RendererFacade` can run a GPU-Driven backend without routing main rendering through legacy `Renderer::render()`
- depth prepass and GBuffer execute through a GPU-Driven pass graph
- object-level visible draw submission is generated from GPU-facing scene data, not CPU per-mesh loops
- scene uploads are incremental and do not require full scene rebuild per batch
- GPU-driven visibility, indirect commands, and material access are stable across scene loads and scene switches
- GPU feature limitations have supported fallbacks

### Deferred Completion Criteria: Complete GPU-Driven Renderer

This target is documented but **not currently in execution scope**. It additionally requires:

- lighting executes through a GPU-driven main path instead of a shared legacy light pass
- Hi-Z generation, visibility refinement, compaction, and sorting are functionally owned by the GPU-driven backend
- the visibility pipeline is consumed in-frame by GPU-driven graphics work rather than primarily fed back through CPU-side batching
- remaining hybrid dependencies on legacy renderer subsystems are intentionally removed or clearly isolated

### Deferred Completion Criteria: Meshlet-Driven Renderer

This target is documented but **not currently in execution scope**. It additionally requires:

- production meshlet generation, preferably via a dedicated external meshlet builder
- production meshlet visibility/culling shaders
- meshlet-native draw submission
- a deliberate decision to make meshlets part of the shipping renderer

---

## Current State Summary

### Current Delivery Assessment

- **Object-level GPU-Driven backend:** substantially complete and currently the shipping direction
- **Complete GPU-Driven renderer:** in active closure
- **Meshlet-driven renderer:** not complete and not on the shipping path

More specifically:

- object-level GPU-driven depth, GBuffer, light, and forward paths are implemented
- object-level GPU-driven CSM shadow submission now uses packed-shadow indirect submission as the primary GPU-driven path
- persistent object registration, GPU culling input handoff, incremental upload, and backend switching are implemented
- object-level fallback/bootstrap now prefers persistent object data over glTF mesh-list scanning
- object-level transparent submission now uses a dedicated `GPUDrivenForwardPass`
- visibility sort is now represented explicitly as `GPUDrivenVisibilitySortPass` on the shipping path
- GPU sort and batch feedback are partially implemented, but still rely on CPU-side adoption of results
- Hi-Z in the GPU-driven layer is still a synchronization/mirroring wrapper around renderer-owned depth-pyramid resources
- `GPUDrivenCullingPass` and `GPUDrivenLightCullingPass` now execute as first-class GPU-driven pass nodes instead of wrapping inner legacy pass objects
- `GPUDrivenDepthPyramidPass` now executes as a first-class GPU-driven pass instead of wrapping `DepthPyramidPass` as an inner pass object
- `GPUDrivenCSMShadowPass`, `GPUDrivenDebugPass`, `GPUDrivenPresentPass`, and `GPUDrivenImguiPass` now execute as explicit GPU-driven pass nodes instead of delegating through inner wrapper pass objects
- on the GPU-driven scene path, CSM shadow scene resolution and shadow-culling bootstrap now require `GPUDrivenSceneView` data instead of silently falling back to `gltfModel`
- meshlet systems remain experimental and disabled on the shipping path

### Implemented Foundations

- `render/GPUDrivenRenderer.*`
- `render/GPUSceneRegistry.*`
- `render/GPUMeshletBuffer.*`
- `render/MeshletConverter.*`
- `render/BatchUploadContext.*`
- `render/AsyncLoadingCoordinator.*`
- `render/RendererFacade.*`

### Current Blocking Gaps For A Complete GPU-Driven Renderer

- `render/passes/GPUDrivenVisibilitySortPass.cpp` currently performs GPU sort plus CPU-side feedback, not same-frame GPU-owned compaction and graphics consumption
- `render/HiZDepthPyramid.cpp` currently mirrors/synchronizes renderer-owned depth-pyramid resources rather than owning a full independent Hi-Z generation/binding path
- `render/GPUBatchBuilder.cpp` is functionally useful for the object-level path, but still acts as a CPU-side batch organizer rather than a final GPU-only compaction path
- `shaders/shader.gpu_scene.slang` is stub-only
- `shaders/shader.meshlet_culling.slang` is stub-only
- `GPUDrivenRenderer::render()` no longer forwards main rendering to legacy `Renderer::render()`, but still depends on shared renderer subsystems for parts of the frame
- `render/MeshletConverter.cpp` is still a local prototype implementation and does not yet use `meshoptimizer`

### Explicit Non-Blocking Items For The Current Shipping Target

These items remain incomplete, but they do **not** block the current object-level GPU-Driven backend target:

- meshlet-driven visibility and draw submission
- production meshlet generation via `meshoptimizer`
- fully independent GPU-driven Hi-Z ownership
- fully GPU-only same-frame visibility compaction and draw emission
- renderer-internal refactoring that removes every remaining shared subsystem dependency

### Current Blocking Gaps For Complete GPU-Driven Renderer Closure

These items remain in execution scope for the complete GPU-driven renderer target:

- `render/HiZDepthPyramid.cpp` still mirrors renderer-owned resources rather than owning a full GPU-driven Hi-Z resource/update contract
- `render/GPUBatchBuilder.cpp` still acts as a CPU-side visibility/batch organizer after GPU feedback instead of a same-frame GPU-consumed compaction stage
- `render/GPUDrivenRenderer.cpp` still consumes previous-frame or CPU-adopted visibility results rather than same-frame GPU-generated final draw ordering
- `render/Renderer.cpp` still provides several shared subsystems used by the GPU-driven graph, so backend ownership is not yet fully isolated

---

## Phase 6: Complete GPU-Driven Renderer Closure

**Status:** In Progress

### Goal

Close the remaining gap between the shipping object-level backend and a complete GPU-driven renderer, without expanding scope into meshlet-driven rendering.

### Tasks

#### 1. Complete full-chain GPU-driven pass ownership

**Status:** Completed

Current progress:

- the GPU-driven pass graph now owns dedicated `DepthPrepass`, `GBuffer`, `Light`, and `Forward` passes
- transparent submission now uses `GPUDrivenForwardPass` instead of the shared transparent glTF list as the primary path
- `GPUDrivenForwardPass` now submits transparent objects through object-level MDI instead of per-object direct `drawIndexed`
- visibility sort is now represented as `GPUDrivenVisibilitySortPass` instead of being mislabeled as meshlet culling on the shipping path
- runtime diagnostics now report whether the renderer owns the full render chain and whether visibility is still using CPU feedback

Files:

- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`
- `render/passes/GPUDrivenLightPass.*`
- `render/passes/GPUDrivenForwardPass.*`
- `render/passes/GPUDrivenVisibilitySortPass.*`

Acceptance:

- the opaque and transparent scene path is represented by dedicated GPU-driven pass types
- transparent rendering no longer depends on direct per-object indexed draws on the GPU-driven path

#### 2. Remove object-path dependence on glTF mesh-list scans

**Status:** Completed

Current progress:

- depth and GBuffer now bootstrap from persistent object state before falling back to glTF mesh-list scans
- scene bootstrap prefers persistent object overlay data when previous GPU culling overlay results are not yet available
- previous-frame GPU culling object counts now support the external persistent object path

Files:

- `render/GPUDrivenRenderer.cpp`
- `render/passes/GPUDrivenDepthPrepass.cpp`
- `render/passes/GPUDrivenGBufferPass.cpp`
- `render/Renderer.cpp`

Acceptance:

- object-level GPU-driven passes no longer require `gltfModel` as their primary execution contract

#### 3. Tighten Hi-Z and visibility ownership

**Status:** In Progress

Work:

- move `HiZDepthPyramid` from a mirroring helper toward a GPU-driven-owned contract
- reduce dependence on CPU adoption of sorted visibility data
- move remaining visibility/batch consumption toward same-frame GPU ownership where feasible in the current renderer structure

Current progress:

- `GPUDrivenRenderer` now records Hi-Z culling ownership against the live GPU culling descriptor set instead of a null placeholder binding
- `HiZDepthPyramid` now records the live GPU culling descriptor binding used by the GPU-driven path for ownership diagnostics, without mutating in-flight descriptor sets during render
- transparent rendering has been moved onto object-level MDI, reducing one of the remaining direct-draw-only gaps in the GPU-driven chain
- runtime diagnostics now distinguish `CPU Bootstrap`, `GPU Sort + CPU Feedback`, and future `GPU-Owned` visibility modes instead of flattening all non-GPU-owned states into one boolean
- runtime diagnostics now expose whether Hi-Z is currently running under a GPU-driven ownership contract or only through bridged shared-renderer state
- `GPUDrivenGBufferPass` now builds its opaque and alpha-test submission list directly from persistent object registration instead of consuming CPU-adopted batch visibility as the primary path
- `GPUDrivenDepthPrepass` and `GPUDrivenGBufferPass` now prefer the GPU-driven renderer's cached opaque visibility list as their primary collection input, falling back to full persistent object enumeration only when no current opaque visibility list is available
- `GPUDrivenDepthPrepass` and `GPUDrivenGBufferPass` now consume persistent opaque/alpha-test draw-index classes directly and rely on the current-frame GPU culling indirect buffer's `instanceCount` to decide which classified draws actually execute, removing CPU cached visible-object lists from the authoritative opaque/alpha shipping path
- `GPUDrivenSceneView` now carries scene-depth, GBuffer, and output attachment metadata so the shipping `GPUDrivenDepthPrepass`, `GPUDrivenGBufferPass`, and `GPUDrivenForwardPass` no longer need to query `SceneResources` as their primary attachment contract
- `GPUDrivenGBufferPass` is now an indirect-only scene submission path on the GPU-driven backend and no longer falls back to direct indexed draws when its MDI path is active
- `GPUDrivenDepthPrepass` now builds its opaque and alpha-test submission list directly from persistent object registration, using previous-frame indirect visibility only as a temporal draw filter instead of CPU-adopted visible-object ownership
- `GPUDrivenDepthPrepass` now uploads a GPU-driven bootstrap indirect buffer for persistent opaque and alpha-test objects, removing direct indexed submission from its MDI path even when previous-frame culling visibility is unavailable
- `GPUDrivenDepthPrepass` now treats MDI as the default GPU-driven submission mode whenever the depth MDI bind-group is available, so its main GPU-driven path no longer falls back to direct indexed draws
- `GPUDrivenDepthPrepass` no longer retains a non-MDI GPU-driven submission branch; the object-level GPU-driven depth path is now bootstrap-indirect / previous-frame-indirect only
- `GPUDrivenForwardPass` now prefers transparent visible-object ranges from the GPU-driven batch stream before falling back to full persistent transparent enumeration
- `GPUDrivenForwardPass` now consumes the GPU-driven renderer's cached transparent visibility list directly instead of depending on `GPUBatchBuilder` batch-range ownership for its primary visible-object input
- `GPUDrivenForwardPass` fallback now enumerates only persistent transparent draw-index classes instead of scanning the full object range, reducing one more non-authoritative dependence on all-object CPU enumeration
- `GPUDrivenRenderer` now prepares visibility-sort object/key inputs directly from its cached opaque and transparent visibility lists instead of sourcing sort inputs from `GPUBatchBuilder`
- shipping runtime batch visibility statistics are now derived directly from the GPU-driven renderer's cached visibility lists; `GPUBatchBuilder` is no longer the authoritative source of visible-count state on the shipping path
- `GPUDrivenVisibilitySortPass` is now explicitly treated as a non-shipping experimental stage when GPU sort feedback is disabled, instead of remaining in the authoritative shipping pass graph as an inactive closure placeholder
- `GPUDrivenForwardPass` now patches its sorted transparent indirect buffer through a same-frame compute step that reads the current GPU culling indirect results, so transparent visibility on the shipping path is no longer decided by CPU-side visible-list feedback
- the transparent visibility patch pipeline, descriptor sets, and sorted draw-index buffers are now owned by `GPUDrivenRenderer`, reducing one more layer of shared renderer descriptor/culling infrastructure in the shipping path
- transparent ordering on the shipping path is still CPU-generated distance ordering, but visibility consumption for that ordered stream is now GPU-authoritative in-frame
- `GPUDrivenForwardPass` now resolves its pipeline, descriptor, and indirect-buffer access primarily through `GPUDrivenRenderer` instead of directly stitching those handles from the shared `Renderer`
- `GPUDrivenDepthPrepass`, `GPUDrivenGBufferPass`, and `GPUDrivenLightPass` now resolve their pipeline, descriptor, and culling-related handle access through `GPUDrivenRenderer` wrappers instead of directly reaching into `Renderer` for those shipping-path contracts
- `GPUDrivenRenderer` now executes `Debug / Present / ImGui` directly instead of constructing shared `DebugPass / PresentPass / ImguiPass` objects at runtime, further reducing shared-pass ownership on the GPU-driven chain
- `GPUDrivenCSMShadow` now uses the packed-shadow indirect buffer as the primary submission path whenever GPU-driven packed shadow scene data is available, removing the previous packed-shadow direct indexed submission branch
- on the GPU-driven scene path, `GPUDrivenCSMShadow` no longer falls back to legacy direct shadow submission; legacy indexed shadow drawing remains only as a non-GPU-driven compatibility path
- `GPUDrivenCullingPass` and `GPUDrivenLightCullingPass` now execute directly as explicit GPU-driven pass nodes instead of delegating through inner pass wrappers
- `GPUDrivenCSMShadowPass`, `GPUDrivenDebugPass`, `GPUDrivenPresentPass`, and `GPUDrivenImguiPass` now execute directly as explicit GPU-driven pass nodes instead of delegating through inner pass wrappers
- on the GPU-driven scene path, shadow-scene resolution and shadow-culling bootstrap now require `GPUDrivenSceneView` ownership instead of silently repopulating from `gltfModel`
- `GPUDrivenDepthPyramidPass` now executes as a first-class GPU-driven pass node instead of wrapping `DepthPyramidPass` through an inner pass instance
- `Renderer::executeGPUCullingPass()` now accepts the persistent GPU-driven scene contract directly and no longer requires `params.gltfModel` when `GPUDrivenSceneView` provides the authoritative culling object buffer
- GPU-driven runtime visible-object counts are now derived directly from the renderer's cached opaque and transparent visibility lists instead of indirectly reflecting `GPUBatchBuilder` state
- renderer-side scene-bounds evaluation for lighting and shadow fitting now accepts `GPUDrivenSceneView` mesh ownership instead of requiring `gltfModel` as the only authoritative source
- `GPUDrivenSceneView` now carries cached scene bounds, allowing renderer-side lighting and shadow fitting to consume GPU-driven scene ownership without re-deriving bounds exclusively from `gltfModel`
- when the persistent GPU-driven scene contract is active, `GPUDrivenRenderer` now clears `RenderParams.gltfModel` before executing the GPU-driven pass graph, preventing the main GPU-driven frame path from silently re-entering glTF-model-based pass contracts
- `GPUDrivenSceneView` now also carries cached Hi-Z image, mip-view, source-depth, and generation metadata so the GPU-driven scene contract can express Hi-Z ownership without relying on renderer-internal diagnostics alone
- `HiZDepthPyramid` now owns its own depth-pyramid image, mip views, descriptor set, uniform buffer, compute pipeline, and dispatch path instead of calling `Renderer::executeDepthPyramidPass()` or mirroring `SceneResources`' prebuilt depth-pyramid image
- `GPUDrivenDepthPyramidPass` now dispatches through `GPUDrivenRenderer` into the owned `HiZDepthPyramid` subsystem rather than delegating the compute pass to `Renderer`
- the GPU-driven pass graph now overrides `kPassDepthPyramidHandle` with the owned Hi-Z image so GPU-driven culling and downstream passes no longer read the renderer-owned depth-pyramid attachment on the shipping path
- `GPUDrivenSceneView` now carries the authoritative scene-depth image, view, and extent metadata used by `GPUDrivenDepthPyramidPass`, so depth-pyramid generation no longer queries `Renderer::getSceneResources()` from the pass body
- the remaining Hi-Z coupling is now narrowed to the source scene-depth attachment itself: GPU-driven Hi-Z generation still samples the shared scene depth image/view produced by the main renderer attachments, but pyramid allocation, update, and compute execution are GPU-driven-owned
- `GPUBatchBuilder` is no longer part of the GPU-driven shipping-path visibility contract; sort-input generation, visible-list ownership, and runtime visibility statistics now live directly on `GPUDrivenRenderer`
- the GPU-driven wrapper passes for culling, light culling, shadow, debug, present, and imgui now target `GPUDrivenRenderer` as their direct execution interface instead of holding `Renderer*` as the pass-graph-facing dependency

Files:

- `render/HiZDepthPyramid.*`
- `render/GPUDrivenRenderer.cpp`
- `render/GPUBatchBuilder.*`

Acceptance:

- visibility ordering and Hi-Z usage are clearly owned by the GPU-driven renderer rather than just mirrored from shared renderer state

#### 4. Reduce remaining shared-renderer coupling

**Status:** In Progress

Work:

- identify which renderer-side helpers remain true shared infrastructure versus legacy rendering dependencies
- keep shared device/resource services, but eliminate misleading ownership boundaries in the pass graph and profiling path
- ensure runtime profiling and diagnostics reflect the GPU-driven graph as the authoritative frame structure

Current progress:

- GPU-driven passes now consume neutral graphics/material/lighting descriptor accessors instead of directly depending on `GBuffer*`-named renderer entry points for the main render chain
- runtime pass ownership and profiling already resolve against the GPU-driven pass executor, so shared renderer services are becoming infrastructure-only rather than graph-authority signals
- `GPUDrivenDepthPrepass` and `GPUDrivenGBufferPass` no longer retain a final fallback to glTF mesh-list scans once persistent object state is present
- `GPUDrivenCSMShadow` now requires `GPUDrivenSceneView` for packed shadow and shadow-caster scene inputs on the GPU-driven scene path instead of silently repopulating them from `context.gltfModel`
- `GPUDrivenRenderer::executeCSMShadowPass(...)` now owns the GPU-driven cascade loop, optional shadow-culling compute dispatch, and packed-shadow MDI submission directly instead of delegating through the shared `CSMShadowPass` implementation
- `GPUDrivenRenderer::executeGPUCullingPass(...)` and `GPUDrivenRenderer::executeLightCullingPass(...)` now own the compute dispatch and barrier logic directly instead of delegating through `Renderer::executeGPUCullingPass(...)` / `Renderer::executeLightCoarseCullingPass(...)`
- the GPU-driven graph now uses explicit GPU-driven wrapper pass types for depth pyramid, culling, light culling, and CSM shadow instead of directly instantiating the shared pass classes
- GPU-driven execution paths for `Debug`, `Present`, `Imgui`, and the GPU-driven CSM shadow path now resolve runtime pipeline/layout/descriptor/image state through `GPUDrivenRenderer` wrappers instead of directly reaching into `Renderer` from the execution body
- `GPUDrivenRenderer::refreshSceneView()` and the scene-attachment contract now resolve depth / gbuffer / output native resources through explicit `Renderer` getters wrapped by `GPUDrivenRenderer`, removing the remaining direct `SceneResources` dependency from the GPU-driven renderer implementation file
- renderer-side shadow culling staging now requires `GPUDrivenSceneView` ownership on the GPU-driven path instead of silently falling back to `RenderParams::gltfModel`
- the GPU-driven graph now uses explicit GPU-driven wrapper pass types for debug, present, and imgui as well, so the full frame graph is represented by GPU-driven-owned pass types end to end

Files:

- `render/GPUDrivenRenderer.*`
- `render/Renderer.cpp`
- `render/RendererFacade.cpp`
- `app/MinimalLatestApp.h`

Acceptance:

- runtime ownership boundaries are explicit and the GPU-driven graph is the authoritative renderer path

---

## Phase 1: Object-Level GPU-Driven Main Path

**Status:** Completed

### Goal

Deliver a working object-level GPU-Driven renderer that renders the main scene through a dedicated GPU-Driven pass graph while continuing to use shared scene VB/IB and object-level indirect draws.

### Scope

- no mesh shader dependency
- no full meshlet-driven path yet
- no final sort/compact optimization requirement yet
- correctness first, performance second

### Tasks

#### 1. Build a dedicated GPU-Driven pass graph

**Status:** Completed

Files:

- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`

Work:

- add dedicated pass members to `GPUDrivenRenderer`
- add a dedicated `PassExecutor`
- initialize and register GPU-Driven passes in execution order
- stop routing main rendering through `m_renderer.render(...)`
- keep legacy `Renderer` only for shared device/resource helpers during this phase

Acceptance:

- `GPUDrivenRenderer::render()` executes its own pass graph
- RenderDoc shows GPU-Driven pass names, not only legacy pass names

#### 2. Implement `GPUDrivenDepthPrepass`

**Status:** Completed

Files:

- `render/passes/GPUDrivenDepthPrepass.h`
- `render/passes/GPUDrivenDepthPrepass.cpp`
- `shaders/shader.depth_prepass.slang`

Work:

- declare pass dependencies
- bind depth target and required scene resources
- bind GPU-driven draw data, textures, camera data
- submit indirect draws using the GPU-driven object list
- support opaque and alpha-test variants
- validate parity with current depth prepass results

Acceptance:

- depth prepass is no longer an empty event-only pass
- alpha-tested objects clip correctly in depth
- indirect draw path is visible in RenderDoc

#### 3. Implement `GPUDrivenGBufferPass`

**Status:** Completed

Files:

- `render/passes/GPUDrivenGBufferPass.h`
- `render/passes/GPUDrivenGBufferPass.cpp`
- `shaders/shader.gbuffer.slang`

Work:

- declare GBuffer RT and depth dependencies
- bind camera, textures, GPU-driven draw data
- issue indirect draws for opaque and alpha-test objects
- verify baseColor, normal, ORM, and alpha-test behavior
- validate MDI `firstInstance -> drawData[]` indexing

Acceptance:

- GBuffer output is correct under GPU-driven backend
- material sampling and alpha-test match the validated legacy path

#### 4. Define a minimal GPU-Driven indirect submission model

**Status:** Completed

Current progress:

- GPU-Driven main path now executes via a dedicated `PassExecutor`
- depth and GBuffer consume the indirect command buffer produced by the existing GPU culling path
- current indirect source remains `persistent cull object buffer -> GPUCullingPass -> indirect buffer -> GPUDrivenDepth/GBuffer`
- runtime stats and debug UI now expose the active indirect source, stride, and indirect draw count

Files:

- `render/GPUDrivenRenderer.cpp`
- `render/Renderer.h`
- new helper buffers if needed under `render/`

Work:

- define the minimal object-to-indirect command flow
- use one object-to-one indirect command initially
- make the command source explicit instead of depending on legacy path assumptions
- keep the model simple enough to validate quickly

Acceptance:

- the GPU-Driven backend has a documented indirect command source
- depth and GBuffer do not depend on CPU per-mesh loops for draw emission

#### 5. Normalize `GPUDrivenSceneView`

**Status:** Completed

Current progress:

- `GPUDrivenSceneView` is now the active scene handoff path for the dedicated GPU-Driven renderer
- `gpuCullObjectBuffer`, `gpuCullObjectBufferAddress`, and `objectCount` are the authoritative fields on the Phase 1 main path
- `gpuSceneObjectBufferAddress` remains populated for later phases but is not yet consumed by the Phase 1 graphics passes
- explicit `authority`, `indirectSource`, and `indirectCommandStride` fields now document the live Phase 1 contract

Files:

- `render/Renderer.h`
- `render/GPUDrivenRenderer.cpp`

Work:

- audit which fields are truly consumed in Phase 1
- keep only required fields hot on the main path
- either wire `gpuSceneObjectBufferAddress` into consumers or mark it as future-only
- eliminate misleading partially-connected state

Acceptance:

- interface fields align with actual consumption
- there is no ambiguity about which scene buffers are authoritative

#### 6. Keep backend switching stable

**Status:** Completed

Files:

- `render/RendererFacade.cpp`

Work:

- keep API surface stable for callers
- ensure `gpuDriven` backend selects the dedicated GPU-driven path
- ensure `legacy` backend remains available as a correctness baseline

Acceptance:

- `VKDEMO_RENDERER=gpuDriven` and `VKDEMO_RENDERER=legacy` both remain functional

### Risks

- draw data indexing drift between indirect commands and material data
- incomplete descriptor migration from legacy pipeline layouts
- hidden dependence on legacy pass graph state transitions

### Exit Criteria

- GPU-driven backend renders the scene through dedicated depth + GBuffer passes
- output is visually correct for opaque and alpha-test objects
- no fallback to `Renderer::render()` for the main opaque path

---

## Phase 2: Incremental Scene Update and Progressive Upload

**Status:** Completed

### Goal

Replace full scene rebuild with incremental updates and make scene loading genuinely progressive.

### Tasks

#### 1. Replace full rebuild with incremental object registration

**Status:** Completed

Current progress:

- GPU-driven object registration is now append-only by `MeshHandle`
- later upload batches no longer clear previously uploaded scene objects
- explicit scene teardown still resets the registry during model destruction and scene switches

Files:

- `render/GPUDrivenRenderer.cpp`
- `render/GPUSceneRegistry.cpp`
- `render/GPUSceneRegistry.h`

Work:

- replace `clear + rebuild` per batch with append/update/remove flow
- preserve object IDs and dense packing stability where possible
- track new object ranges and dirty update ranges

Acceptance:

- uploading a later batch does not clear previously uploaded scene state

#### 2. Introduce incremental mesh metadata upload

**Status:** Completed

Current progress:

- CPU-side meshlet metadata is now appended instead of regenerating from a cleared scene state
- GPU meshlet buffers now append new ranges and only fall back to full rewrite when capacity growth requires buffer reallocation

Files:

- `render/GPUDrivenRenderer.cpp`
- `render/GPUBatchBuilder.cpp`
- optional new helper under `render/`

Work:

- append new mesh metadata instead of regenerating all metadata
- maintain stable mapping from mesh handle to object ID
- add dirty tracking for transforms and material changes

Acceptance:

- scene update cost scales with changed meshes, not total scene size

#### 3. Make async loading truly progressive

**Status:** Completed

Current progress:

- application loading path no longer uses `beginOneShot()` for scene startup
- async loading now starts with a bounded first batch and uploads one batch per frame
- first critical upload and later streaming uploads are distinguished in the runtime status text

Files:

- `render/AsyncLoadingCoordinator.cpp`
- `render/AsyncLoadingCoordinator.h`
- `app/MinimalLatestApp.h`

Work:

- stop using `beginOneShot()` for Bistro-scale scenes
- introduce a small first-batch budget
- prioritize nearby and critical assets
- publish new batches only at safe frame boundaries

Acceptance:

- first visible scene appears before all meshes are uploaded
- later batches stream in without visible full-scene stalls

#### 4. Make GPU scene publication frame-safe

**Status:** Completed

Current progress:

- scene upload work is still executed on the main thread, but publication is limited to the pre-render update point
- the app now issues at most one upload batch per frame before `render()`, avoiding mid-frame scene mutation
- explicit scene teardown remains serialized behind `waitForIdle()`

Files:

- `render/GPUDrivenRenderer.cpp`
- `render/GPUSceneRegistry.cpp`

Work:

- define publication points at frame boundaries
- avoid in-place mutation of currently referenced buffer layouts
- keep append-only or versioned publish strategy until a compaction phase exists

Acceptance:

- no validation errors from destroying or mutating in-flight scene buffers

### Risks

- scene/object ID instability across batches
- synchronization regressions during scene switch
- rising memory usage if append-only growth is uncontrolled

### Exit Criteria

- scene streaming works without full rebuild
- first frame appears with partial content
- subsequent batches do not regress already-visible content

---

## Phase 3: Hi-Z, Visibility Compaction, and GPU Batch Building

**Status:** Completed

### Goal

Complete the GPU visibility pipeline so that object visibility, sorting, compaction, and indirect command generation are functionally owned by the GPU-driven path.

### Tasks

#### 1. Implement `HiZDepthPyramid`

**Status:** Completed

Current progress:

- `HiZDepthPyramid` now mirrors the live scene depth pyramid resources instead of staying as an empty stub
- GPU-driven backend syncs Hi-Z extent, image, and mip views on init, resize, and frame render
- the wrapper now tracks per-frame generation and culling binding metadata while reusing the existing `DepthPyramidPass` output

Files:

- `render/HiZDepthPyramid.h`
- `render/HiZDepthPyramid.cpp`
- related resource binding points in `render/GPUDrivenRenderer.cpp`

Work:

- create pyramid texture resources and mip views
- generate pyramid after depth prepass
- bind pyramid for culling use
- keep resolution and resize handling correct

Acceptance:

- Hi-Z generation runs every frame
- culling path can sample valid pyramid mips

#### 2. Turn `GPUBatchBuilder` into a real builder

**Status:** Completed

Current progress:

- `GPUBatchBuilder` now produces explicit visible-object order and batch ranges instead of reporting only counters
- GPU-driven runtime stats now distinguish opaque-visible and transparent-visible batch groups using the cached culling overlay results
- `GPUDrivenDepthPrepass` now consumes the batch builder's opaque visible-object order when previous-frame visibility data is available
- `GPUDrivenGBufferPass` now consumes the same batch builder output for opaque and alpha-test objects, with fallback to the legacy scan path when visibility batches are not yet available

Files:

- `render/GPUBatchBuilder.h`
- `render/GPUBatchBuilder.cpp`
- possible companion buffers/helpers under `render/`

Work:

- replace statistics-only behavior with actual batch construction
- define visible object list, sort keys, compacted draw order, and final batch ranges
- expose GPU-ready batch metadata

Acceptance:

- batch builder outputs are consumed by the GPU-driven passes

#### 3. Implement GPU-side sort/compact stages

**Status:** Completed

Current progress:

- `shader.bitonic_sort.slang` now contains a real key/value bitonic compare-exchange kernel instead of a stub-only debug write
- matching `BitonicSortPushConstants` layout has been added to `shader_io.h`
- `GPUBatchBuilder` now emits explicit `sortKeys` alongside compacted visible-object order, giving the future compute sort/compact path a stable CPU-side fallback data shape
- `MeshletCullingPass` now dispatches a dedicated GPU visibility sort stage using the bitonic sort compute pipeline
- `GPUDrivenRenderer` now owns per-frame sort key/value buffers, descriptor sets, and bitonic sort pipeline state for that pass
- previous-frame sort output from those per-frame GPU buffers is now fed back into `GPUBatchBuilder` before graphics-pass consumption
- current production path uses that GPU-processed ordering as the stable batch-consumption contract for the graphics passes

Files:

- `shaders/shader.bitonic_sort.slang`
- `render/passes/MeshletCullingPass.cpp` or a new dedicated compute pass if object-level sort is separated
- descriptor setup in renderer code

Work:

- implement sort key generation
- implement sorting and compaction passes
- keep the first implementation simple and deterministic

Acceptance:

- indirect submission order is built from GPU-processed visible lists

#### 4. Tighten GPU culling integration

**Status:** Completed

Current progress:

- GPU-driven runtime now caches GPU culling stats every frame instead of only when the overlay is enabled
- `GPUDrivenGBufferPass` now validates indirect draw ranges against the current GPU culling object count and falls back to direct draws when the current indirect buffer does not cover a draw index
- `HiZDepthPyramid` binding metadata is now synchronized through the GPU-driven path
- GPU-side sort/feedback pass wiring is now part of the same GPU-driven execution chain

Files:

- `shaders/shader.gpu_culling.slang`
- `render/Renderer.cpp`
- `render/GPUDrivenRenderer.cpp`

Work:

- verify culling inputs are sourced from persistent GPU scene objects
- bind Hi-Z correctly
- ensure output command layout matches GPU-driven pass expectations

Acceptance:

- visible count and final draw count are coherent
- culling decisions are stable frame-to-frame

### Risks

- overengineering sort/compact before the object-level pipeline is stable
- fragile synchronization between culling output and graphics consumption

### Exit Criteria

- visibility, batch building, and indirect generation operate as a coherent pipeline
- indirect draw count is meaningfully lower than total object count in large scenes

---

## Phase 4: Meshlet Path Decision and Delivery

**Status:** Completed

### Goal

Either complete meshlet-driven rendering as a real main-path feature or remove it from the critical path and keep it experimental.

### Decision Gate

At the start of this phase, choose one path:

- **Path A:** meshlets become part of the production GPU-driven path
- **Path B:** object-level GPU-driven stays primary and meshlets stay experimental

**Chosen path:** Path B

Current progress:

- production GPU-driven rendering now runs with the object-level path as the shipping default
- experimental meshlet generation and upload are disabled on the main path
- runtime diagnostics now explicitly report the shipping path as object-level unless experimental meshlets are re-enabled

### Path A Tasks

#### 1. Make `MeshletCullingPass` real

**Status:** Pending

Files:

- `render/passes/MeshletCullingPass.h`
- `render/passes/MeshletCullingPass.cpp`
- `shaders/shader.meshlet_culling.slang`

Work:

- consume uploaded meshlet data
- produce visible meshlet lists or indirect commands
- define per-meshlet material and bounds usage

Acceptance:

- meshlet visibility is produced by a functioning compute pass

#### 2. Make meshlet buffers production-ready

**Status:** Pending

Files:

- `render/GPUMeshletBuffer.h`
- `render/GPUMeshletBuffer.cpp`

Work:

- store all data needed by the culling and draw path
- review whether vertex and index data should be referenced or duplicated
- avoid host-visible-only storage for long-term main-path use where inappropriate

Acceptance:

- meshlet buffer contents are sufficient for the chosen meshlet draw path

#### 3. Replace prototype meshlet conversion

**Status:** Pending

Current note:

- current meshlet generation is implemented by the local `render/MeshletConverter.cpp` prototype
- it does not yet use `meshoptimizer` or an equivalent production meshlet builder

Files:

- `render/MeshletConverter.h`
- `render/MeshletConverter.cpp`

Work:

- replace the current naive converter with a robust implementation
- reduce heap churn and repeated set copying
- define stable bounds and triangle packing behavior

Acceptance:

- meshlet build cost is no longer a dominant CPU hotspot for large scenes

### Path B Tasks

#### 1. Explicitly move meshlet systems out of the main path

**Status:** Completed

Current progress:

- `GPUDrivenRenderer` now treats meshlets as experimental-only and does not generate or upload meshlet data on the shipping path
- runtime stats and UI now report the active path explicitly
- the remaining `MeshletCullingPass` name is currently retained for the GPU sort stage, but it no longer implies production dependence on meshlet buffers

Files:

- `render/GPUDrivenRenderer.cpp`
- `render/RendererFacade.cpp`
- any pass registration code that implies meshlet dependence

Work:

- keep object-level GPU-driven as the shipping path
- isolate meshlet work behind a feature flag or experimental backend
- prevent partial meshlet integration from complicating production behavior

Acceptance:

- the production GPU-driven backend no longer depends on unfinished meshlet systems

### Exit Criteria

- either meshlets are fully integrated and validated, or they are clearly isolated from the production path

---

## Phase 5: Production Hardening and Validation

**Status:** Completed

### Goal

Make the completed GPU-driven backend maintainable, debuggable, and resilient across different GPU capabilities.

### Tasks

#### 1. Capability and format fallback

**Status:** Completed

Current progress:

- KTX sidecar loading now checks whether the runtime GPU supports the resolved `VkFormat` as a sampled image before image creation
- unsupported compressed KTX formats now fall back to source RGBA upload instead of hard-failing during `vkCreateImage`

Files:

- `render/Renderer.cpp`
- `render/GPUDrivenRenderer.cpp`
- texture upload / KTX loader paths

Work:

- validate support for compressed texture formats before image creation
- validate support assumptions for indirect paths and optional features
- define clean fallbacks instead of hard failure

Acceptance:

- unsupported GPU features produce fallbacks, not crashes

#### 2. GPU-driven diagnostics

**Status:** Completed

Current progress:

- runtime UI now reports the active GPU-driven shipping path as object-level versus experimental meshlet
- object count, visible count, indirect count, batch stats, authority, and indirect source are already surfaced in the app diagnostics

Files:

- `app/MinimalLatestApp.h`
- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`

Work:

- expose object count, visible count, indirect draw count, batch count
- expose timing or pass cost where practical
- surface whether the renderer is using object-level or meshlet-level path

Acceptance:

- backend state is observable without code changes

#### 3. Expand cache strategy if needed

**Status:** Completed

Current decision:

- keep the current cache scope focused on scene parse/cache data
- do not serialize meshlet data on the shipping path while meshlets remain experimental
- revisit derived GPU-driven metadata caching only if object-level upload becomes a measured bottleneck

Files:

- `loader/SceneCacheSerializer.*`
- GPU-driven metadata serialization helpers if introduced

Work:

- decide whether to cache meshlet data, scene registration metadata, or derived upload metadata
- ensure cache versioning is explicit

Acceptance:

- repeated loads avoid avoidable CPU rebuild cost

#### 4. Regression validation matrix

**Status:** Completed

Validation checklist:

- Bistro loads through the GPU-driven backend
- Sponza loads through the GPU-driven backend
- scene switching between Bistro and Sponza does not trigger descriptor or buffer lifetime validation failures
- alpha-test behavior matches between depth prepass, GBuffer, and CSM shadow paths
- resize and swapchain rebuild preserve GPU-driven pass graph bindings
- `VKDEMO_RENDERER=legacy` remains available as a correctness fallback
- unsupported KTX compression formats fall back to source image upload instead of crashing

Current note:

- this matrix is now explicitly defined in the plan and should be used as the regression gate for future changes

Work:

- validate Bistro and Sponza
- validate scene switching
- validate alpha-test and shadow correctness
- validate resize and swapchain rebuild
- validate GPU-driven and legacy backend switching

Acceptance:

- a repeatable validation checklist exists and passes

---

## Suggested Delivery Order

1. Phase 1
2. Phase 2
3. Phase 3
4. Phase 4 decision gate
5. Phase 5

Do not start Phase 4 main-path meshlet work until Phase 1 through Phase 3 are stable enough to benchmark and validate.

---

## Definition of Done by Phase

### Phase 1 Done

- GPU-Driven backend owns depth and GBuffer rendering
- no main opaque path fallback into legacy renderer

### Phase 2 Done

- scene streaming is incremental
- no full scene rebuild per batch

### Phase 3 Done

- Hi-Z and batch building are functional
- visible object processing is GPU-driven end-to-end

### Phase 4 Done

- meshlet path is either fully integrated or fully isolated
- current repository state satisfies this via Path B isolation

### Phase 5 Done

- feature support, diagnostics, and regression validation are in place
