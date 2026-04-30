# GPU-Driven Rendering Architecture Design

**Date:** 2026-04-30
**Project:** VKDemo
**Author:** Claude (brainstorming session with user)

---

## Overview

This design specifies a new `GPUDrivenRenderer` class that implements modern GPU-Driven rendering techniques alongside the existing `Renderer` class. The goal is learning/research-focused implementation of four key GPU-Driven subsystems.

### Motivation

- **Learning:** Understand modern GPU-Driven rendering architecture
- **Research:** Compare CPU-driven vs GPU-Driven performance
- **Education:** Clear, understandable implementations without production complexity

### Design Approach

**Approach B: New Renderer Variant** - Create a separate `GPUDrivenRenderer` class that implements the full pipeline. Keep `Renderer` for legacy path, switch via configuration flag.

**Benefits:**
- Clean separation of concerns
- No risk to existing functionality
- Easy A/B performance testing
- Two systems to compare and learn from

---

## High-Level Architecture

```
Application Layer
├── MinimalApp (legacy) → Renderer (DrawStream + CPU)
├── GPUDrivenApp (new)  → GPUDrivenRenderer (GPU Indirect Commands)
│
└── Both renderers share:
    └── RHI Layer (Device, Swapchain, CommandList)
```

**Key Design Points:**
- Both renderers use same RHI abstractions
- `GPUDrivenRenderer` has its own pass implementations
- Resources like `MeshPool` and textures can be shared
- Switchable at runtime or compile-time

---

## Phase 1: GPU Scene Persistence

### Goal

Upload scene data once at load time. Only update when transforms/materials change. Eliminate per-frame object buffer uploads.

### Core Component: GPUSceneRegistry

```cpp
class GPUSceneRegistry {
public:
    // Object registration (load-time)
    uint32_t registerObject(MeshHandle mesh, MaterialHandle material, const glm::mat4& transform);
    void removeObject(uint32_t objectID);

    // Transform updates (runtime)
    void updateTransform(uint32_t objectID, const glm::mat4& newTransform);

    // GPU access
    uint64_t getBufferAddress() const;  // Buffer Device Address for shaders
    uint32_t getObjectCount() const;

private:
    // Device-local buffer (persistent)
    utils::Buffer m_objectBuffer;

    // Host-visible staging (for updates)
    utils::Buffer m_updateBuffer;
    void* m_mappedUpdateData;

    // Object tracking
    HandlePool<uint32_t, ObjectRecord> m_objectPool;
};
```

### GPUSceneObject Structure

```cpp
// shaders/shader_io.h
struct GPUSceneObject {
    mat4x3 worldMatrix;      // 48 bytes - transform (saves 16 vs mat4)
    vec4   boundsSphere;     // 16 bytes - xyz=center, w=radius
    uint32 materialIndex;    // 4 bytes - material table index
    uint32 meshIndex;        // 4 bytes - mesh data index
    uint32 flags;            // 4 bytes - visibility flags
    uint32 _padding;         // 4 bytes
};  // Total: 80 bytes per object
```

### Shader Access

```glsl
// Bindless access via Buffer Device Address
[[vk::binding(0, 1)]]
ConstantBuffer<GPUSceneInfo> sceneInfo;

struct GPUSceneInfo {
    uint64_t objectBufferAddress;
    uint32_t objectCount;
};

// Access in shader
GPUSceneObject obj = sceneInfo.objectBufferAddress[objectId];
```

### Requirements

- `VK_KHR_buffer_device_address` extension
- Device-local memory for object buffer
- Host-visible staging for transform updates

### Implementation Time

~1-2 weeks

---

## Phase 2: Hi-Z Occlusion Culling

### Goal

GPU hierarchical depth buffer for visibility testing. Single frame latency: culling at Frame N uses pyramid from Frame N-1.

### Core Component: HiZDepthPyramid

```cpp
class HiZDepthPyramid {
public:
    void init(rhi::Device& device, VkExtent2D size);
    void generate(VkCommandBuffer cmd, TextureHandle sourceDepth);
    void bindForCulling(VkDescriptorSet set, uint32_t binding);
    uint32_t getMipCount() const;

private:
    TextureHandle m_pyramidTexture;
    utils::Buffer m_uniformBuffer;
    uint32_t m_mipCount;
};
```

### Pipeline Flow

```
Frame N:
1. Depth Prepass → writes depth to GBuffer
2. Depth Pyramid Generation → compute shader reduces to mip chain
3. Main Pass → uses pyramid from Frame N-1 for culling
```

### Occlusion Shader Logic

```glsl
bool isOccluded(vec3 worldCenter, float radius) {
    // Project bounding sphere to screen space
    vec4 viewPos = mul(viewMatrix, vec4(worldCenter, 1.0));

    // Near plane check
    float nearestZ = -viewPos.z - radius;
    if (nearestZ <= 0.0) return false;

    // Calculate screen footprint
    vec2 screenPos = clipPos.xy / clipPos.w;
    float projectedRadius = radius * projectionScale / nearestZ;

    // Select mip level based on footprint
    uint mipLevel = clamp(uint(log2(projectedRadius * screenSize)) - 3, 0, maxMip - 1);

    // Sample depth pyramid
    float hizDepth = depthPyramid[mipLevel].Sample(centerCoord);

    // Compare depths
    float objectDepth = depthFromViewZ(nearestZ);
    return objectDepth < hizDepth - epsilon;
}
```

### Integration

- Extends existing `DepthPyramidPass`
- Adds occlusion testing to `GPUCullingPass` shader
- Single mip chain storage

### Limitations

- First frame: no previous pyramid (all objects visible)
- Fast camera movement may have minor errors

### Implementation Time

~1 week

---

## Phase 3: GPU Batch Builder

### Goal

Sort visible objects by material index on GPU. Output sorted indirect command list. Reduce material state changes.

### Core Component: GPUBatchBuilder

```cpp
class GPUBatchBuilder {
public:
    void init(rhi::Device& device, uint32_t maxObjects);
    void buildBatches(VkCommandBuffer cmd, uint32_t visibleCount);
    BufferHandle getSortedIndirectBuffer() const;

private:
    utils::Buffer m_sortKeysBuffer;      // Material indices
    utils::Buffer m_sortedIndicesBuffer; // Sorted object indices
    utils::Buffer m_sortedIndirectBuffer;// Sorted draw commands
    PipelineHandle m_sortPipeline;
    PipelineHandle m_compactPipeline;
};
```

### Algorithm: Bitonic Sort

```glsl
[numthreads(64, 1, 1)]
void bitonicSort(uint3 dispatchId) {
    uint index = dispatchId.x;

    for (uint k = 2; k <= count; k *= 2) {
        for (uint j = k / 2; j > 0; j /= 2) {
            uint i = index ^ j;

            if (i > index && i < count) {
                uint keyA = keys[index];
                uint keyB = keys[i];

                bool ascending = ((index & k) == 0);
                bool shouldSwap = (keyA > keyB) == ascending;

                if (shouldSwap) {
                    swap(keys[index], keys[i]);
                    swap(indices[index], indices[i]);
                }
            }
            barrier();
        }
    }
}
```

### Pipeline Steps

1. **Extract Sort Keys** → compute shader reads material indices from visible objects
2. **Bitonic Sort** → parallel O(n log² n) sorting (~100 iterations for 10K objects)
3. **Compact Commands** → reorder indirect commands by sorted indices

### Rendering with Sorted Commands

```cpp
// Material binding only when material changes
uint32_t currentMaterial = INVALID;
for (uint32_t i = 0; i < visibleCount; ++i) {
    uint32_t matIdx = objectBuffer[sortedIndices[i]].materialIndex;

    if (matIdx != currentMaterial) {
        bindMaterial(matIdx);
        currentMaterial = matIdx;
    }

    drawIndexedIndirect(sortedIndirectBuffer, i);
}
```

### Implementation Time

~1-2 weeks

---

## Phase 4: Meshlet Rendering

### Goal

Sub-mesh granularity culling. Split meshes into small chunks (meshlets), cull each independently.

### What is a Meshlet

- Small chunk of mesh geometry (64-256 triangles)
- Each meshlet has own bounding sphere
- Fine-grained culling: only draw visible meshlets

### Core Components

```cpp
struct Meshlet {
    vec4   boundsSphere;     // xyz=center, w=radius
    uint32 vertexOffset;     // Index into packed vertex buffer
    uint32 indexOffset;      // Index into meshlet index buffer
    uint32 triangleCount;    // Number of triangles
    uint32 materialIndex;    // Material for this meshlet
};  // 32 bytes

class GPUMeshletBuffer {
public:
    void init(rhi::Device& device, uint32_t maxMeshlets);
    void uploadMeshlets(VkCommandBuffer cmd, const std::vector<Meshlet>& meshlets);
    uint64_t getMeshletDataAddress() const;

private:
    utils::Buffer m_meshletDataBuffer;
    utils::Buffer m_meshletVertexBuffer;
    utils::Buffer m_meshletIndexBuffer;
    utils::Buffer m_meshInfoBuffer;
};

class MeshletConverter {
public:
    static MeshletConversionResult convert(const Mesh& mesh,
        uint32_t maxVertices = 64, uint32_t maxTriangles = 124);
};
```

### Meshlet Culling Shader

```glsl
[numthreads(64, 1, 1)]
void meshletCullingMain(uint3 dispatchId) {
    uint meshletIndex = dispatchId.x;
    if (meshletIndex >= totalMeshletCount) return;

    Meshlet meshlet = meshletDataBuffer[meshletIndex];

    // Frustum culling
    vec3 center = meshlet.boundsSphere.xyz;
    float radius = meshlet.boundsSphere.w;
    bool frustumVisible = isInsideFrustum(center, radius);

    // Occlusion culling
    bool occlusionVisible = !isOccluded(center, radius);

    // Write indirect command
    DrawIndexedIndirectCommand cmd;
    cmd.indexCount = meshlet.triangleCount * 3;
    cmd.instanceCount = (frustumVisible && occlusionVisible) ? 1 : 0;
    cmd.firstIndex = meshlet.indexOffset;
    cmd.vertexOffset = meshlet.vertexOffset;
    cmd.firstInstance = 0;

    meshletIndirectBuffer[meshletIndex] = cmd;
}
```

### Meshlet Conversion (Load-time)

```cpp
// Greedy clustering algorithm
MeshletConversionResult convertMeshToMeshlets(const Mesh& mesh) {
    // 1. Build adjacency graph from mesh triangles
    // 2. Greedy clustering: add triangles until limits hit
    // 3. Compute bounding sphere per meshlet
    // 4. Output: packed vertex/index buffers
}
```

### Note

This is compute-based approach (works on any GPU). Advanced mesh shader approach would use `VK_EXT_mesh_shader` for tighter integration.

### Implementation Time

~2 weeks

---

## New Files to Create

```
render/GPUDrivenRenderer.h/.cpp       - Main renderer class
render/GPUSceneRegistry.h/.cpp        - Phase 1
render/HiZDepthPyramid.h/.cpp         - Phase 2
render/GPUBatchBuilder.h/.cpp         - Phase 3
render/MeshletConverter.h/.cpp        - Phase 4
render/GPUMeshletBuffer.h/.cpp        - Phase 4

render/passes/GPUDrivenDepthPrepass.h/.cpp
render/passes/GPUDrivenGBufferPass.h/.cpp
render/passes/GPUDrivenLightPass.h/.cpp
render/passes/MeshletCullingPass.h/.cpp

shaders/shader.gpu_scene.slang       - Scene buffer types
shaders/shader.meshlet_culling.slang - Meshlet culling compute
shaders/shader.bitonic_sort.slang    - Batch builder sort
```

---

## Testing Strategy

Each phase tested independently:

### Phase 1 Tests
- Verify Buffer Device Address works
- Test transform updates (staging → device copy)
- Compare object count vs CPU-side

### Phase 2 Tests
- Visual debug overlay (show culled/visible)
- Compare culling stats vs frustum-only
- Test various camera angles

### Phase 3 Tests
- Verify sort correctness (visual material grouping)
- Measure state change reduction
- Profile sort shader performance

### Phase 4 Tests
- Meshlet count vs triangle count comparison
- Visual meshlet bounding spheres (debug)
- Compare draw call count vs traditional

### A/B Performance Comparison
- Same scene with Renderer vs GPUDrivenRenderer
- Measure: CPU time, GPU time, draw call count
- Document results

---

## Integration with Existing Code

| Existing Component | GPU-Driven Use |
|-------------------|----------------|
| RHI Layer | Shared: Device, CommandList, Swapchain |
| HandlePool | Pattern reuse for GPU object IDs |
| DepthPyramidPass | Extend for Hi-Z Phase 2 |
| GPUCullingPass | Add occlusion testing Phase 2 |
| MeshPool | Add meshlet conversion Phase 4 |
| TracyProfiling | GPU timing zones for all phases |

---

## Timeline

| Phase | Duration | Dependencies |
|-------|----------|--------------|
| Phase 1 | ~1-2 weeks | None |
| Phase 2 | ~1 week | Phase 1 |
| Phase 3 | ~1-2 weeks | Phase 2 |
| Phase 4 | ~2 weeks | Phase 3 |
| **Total** | **~5-7 weeks** | |

---

## Requirements

- `VK_KHR_buffer_device_address` extension
- `VK_EXT_descriptor_indexing` (already have for bindless)
- Compute shader support (already have)
- Optional: `VK_EXT_mesh_shader` for advanced Phase 4

---

## Success Criteria

1. All four phases implemented and tested
2. A/B performance comparison completed
3. Visual debug overlays working
4. Documentation of learnings and results
5. No regression to existing Renderer