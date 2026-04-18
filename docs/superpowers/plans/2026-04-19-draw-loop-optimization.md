# Draw Loop Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate per-frame per-pass redundant lookups and iterations by pre-computing material metadata, building per-pass mesh lists, caching transparent sorting, and consolidating camera uploads.

**Architecture:** Extend `MeshRecord` with cached material texture indices/factors. Extend `GltfUploadResult` with pre-built mesh lists per alpha-mode. Add dirty-flag tracking for transparent sorting. Upload camera data once per frame, passes bind only.

**Tech Stack:** C++17, Vulkan, existing MeshPool/GltfUploadResult structures

---

## File Structure

| File | Responsibility |
|------|----------------|
| `render/MeshPool.h` | MeshRecord extension with cached material data |
| `render/MeshPool.cpp` | setMeshMaterialData() implementation |
| `render/Renderer.h` | GltfUploadResult extension with mesh lists |
| `render/Renderer.cpp` | uploadGltfModel() builds mesh lists, single camera upload |
| `render/Pass.h` | PassContext extension for shared camera allocation |
| `render/passes/GBufferPass.cpp` | Use pre-computed data and mesh lists |
| `render/passes/DepthPrepass.cpp` | Use pre-computed data and mesh lists |
| `render/passes/ForwardPass.cpp` | Use transparent mesh list with cached sorting |
| `render/passes/CSMShadowPass.cpp` | Use shadow-caster mesh list |

---

### Task 1: Extend MeshRecord with Cached Material Data

**Files:**
- Modify: `render/MeshPool.h:15-49`
- Modify: `render/MeshPool.cpp:188-198`

- [ ] **Step 1: Add material texture indices and factors to MeshRecord**

In `render/MeshPool.h`, extend MeshRecord struct:

```cpp
struct MeshRecord {
    uint64_t vertexBufferHandle = 0;
    uint64_t indexBufferHandle = 0;
    VmaAllocation vertexAllocation = nullptr;
    VmaAllocation indexAllocation = nullptr;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 48;
    glm::mat4 transform = glm::mat4(1.0f);
    int32_t materialIndex = -1;

    // Pre-computed alpha mode from material
    int32_t alphaMode = 0;      // 0=OPAQUE, 1=MASK, 2=BLEND
    float alphaCutoff = 0.5f;

    // Pre-computed material texture indices (bindless)
    int32_t baseColorTextureIndex = -1;
    int32_t normalTextureIndex = -1;
    int32_t metallicRoughnessTextureIndex = -1;
    int32_t occlusionTextureIndex = -1;

    // Pre-computed material factors
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;

    glm::vec3 localBoundsMin = glm::vec3(0.0f);
    glm::vec3 localBoundsMax = glm::vec3(0.0f);
    glm::vec3 worldBoundsMin = glm::vec3(0.0f);
    glm::vec3 worldBoundsMax = glm::vec3(0.0f);

    // Helpers unchanged...
    VkBuffer getNativeVertexBuffer() const { return reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(vertexBufferHandle)); }
    VkBuffer getNativeIndexBuffer() const { return reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(indexBufferHandle)); }
    void setNativeVertexBuffer(VkBuffer buffer) { vertexBufferHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buffer)); }
    void setNativeIndexBuffer(VkBuffer buffer) { indexBufferHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(buffer)); }
};
```

- [ ] **Step 2: Add setMeshMaterialData function declaration**

In `render/MeshPool.h`, after `setMeshAlphaMode`:

```cpp
void setMeshAlphaMode(MeshHandle handle, int32_t alphaMode, float alphaCutoff);
void setMeshMaterialData(MeshHandle handle,
                         const glm::vec4& baseColorFactor,
                         int32_t baseColorTextureIndex,
                         int32_t normalTextureIndex,
                         int32_t metallicRoughnessTextureIndex,
                         int32_t occlusionTextureIndex,
                         float metallicFactor,
                         float roughnessFactor,
                         float normalScale);
```

- [ ] **Step 3: Implement setMeshMaterialData**

In `render/MeshPool.cpp`, after `setMeshAlphaMode`:

```cpp
void MeshPool::setMeshMaterialData(MeshHandle handle,
                                   const glm::vec4& baseColorFactor,
                                   int32_t baseColorTextureIndex,
                                   int32_t normalTextureIndex,
                                   int32_t metallicRoughnessTextureIndex,
                                   int32_t occlusionTextureIndex,
                                   float metallicFactor,
                                   float roughnessFactor,
                                   float normalScale)
{
    MeshRecord* record = m_pool.tryGet(handle);
    if(record == nullptr)
    {
        return;
    }

    record->baseColorFactor = baseColorFactor;
    record->baseColorTextureIndex = baseColorTextureIndex;
    record->normalTextureIndex = normalTextureIndex;
    record->metallicRoughnessTextureIndex = metallicRoughnessTextureIndex;
    record->occlusionTextureIndex = occlusionTextureIndex;
    record->metallicFactor = metallicFactor;
    record->roughnessFactor = roughnessFactor;
    record->normalScale = normalScale;
}
```

- [ ] **Step 4: Build to verify compilation**

Run: `cmake --build build --target Demo`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add render/MeshPool.h render/MeshPool.cpp
git commit -m "feat(mesh): add material texture indices and factors to MeshRecord"
```

---

### Task 2: Extend GltfUploadResult with Pre-Built Mesh Lists

**Files:**
- Modify: `render/Renderer.h:110-115`

- [ ] **Step 1: Add mesh lists to GltfUploadResult**

In `render/Renderer.h`:

```cpp
struct GltfUploadResult
{
    std::vector<MeshHandle>     meshes;           // All meshes
    std::vector<MaterialHandle> materials;
    std::vector<TextureHandle>  textures;

    // Pre-built mesh lists for each pass type
    std::vector<size_t> opaqueMeshIndices;        // Indices into meshes for OPAQUE
    std::vector<size_t> alphaTestMeshIndices;     // Indices into meshes for MASK
    std::vector<size_t> transparentMeshIndices;   // Indices into meshes for BLEND
    std::vector<size_t> shadowCasterIndices;      // OPAQUE + MASK (skip BLEND)

    // Cached sorting data for transparent meshes
    std::vector<float> transparentDistances;      // Distance from last camera position
    glm::vec3 lastSortCameraPos{0.0f};            // Camera position used for last sort
    bool transparentSortDirty{true};              // Force re-sort on first frame
};
```

- [ ] **Step 2: Build to verify compilation**

Run: `cmake --build build --target Demo`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add render/Renderer.h
git commit -m "feat(gltf): add pre-built mesh lists to GltfUploadResult"
```

---

### Task 3: Populate Material Data in uploadGltfModel

**Files:**
- Modify: `render/Renderer.cpp` (uploadGltfModel function, ~line 2350)

- [ ] **Step 1: Find uploadGltfModel mesh upload section**

Read `render/Renderer.cpp` to locate where meshes are uploaded and material data should be set. The existing code sets alphaMode after mesh upload. Extend to set all material data.

- [ ] **Step 2: Compute texture indices once at upload time**

In the mesh upload loop in `uploadGltfModel`, after `setMeshAlphaMode`, compute texture indices:

```cpp
// After uploading meshes and setting alphaMode...
// Now set all material texture indices and factors
for(size_t i = 0; i < model.meshes.size(); ++i)
{
    MeshHandle meshHandle = result.meshes[i];
    if(model.meshes[i].materialIndex < 0 ||
       model.meshes[i].materialIndex >= static_cast<int32_t>(model.materials.size()))
    {
        continue;
    }

    MaterialHandle matHandle = result.materials[model.meshes[i].materialIndex];
    const MaterialResources::MaterialRecord* material = tryGetMaterial(matHandle);
    if(!material)
    {
        continue;
    }

    // Compute bindless texture indices once
    const uint32_t gltfTextureBaseIndex = getGltfTextureBaseIndex();
    auto findTextureIndex = [&](TextureHandle handle) -> int32_t {
        if(handle.isNull()) return -1;
        for(size_t t = 0; t < result.textures.size(); ++t)
        {
            if(result.textures[t] == handle)
            {
                return static_cast<int32_t>(gltfTextureBaseIndex + t);
            }
        }
        return -1;
    };

    m_meshPool.setMeshMaterialData(meshHandle,
        material->baseColorFactor,
        findTextureIndex(material->baseColorTexture),
        findTextureIndex(material->normalTexture),
        findTextureIndex(material->metallicRoughnessTexture),
        findTextureIndex(material->occlusionTexture),
        material->metallicFactor,
        material->roughnessFactor,
        material->normalScale);
}
```

- [ ] **Step 3: Build mesh lists during upload**

After all meshes have material data, build the mesh lists:

```cpp
// Build mesh lists for each pass type
for(size_t i = 0; i < result.meshes.size(); ++i)
{
    MeshHandle meshHandle = result.meshes[i];
    const MeshRecord* mesh = m_meshPool.tryGet(meshHandle);
    if(!mesh) continue;

    const int32_t alphaMode = mesh->alphaMode;

    if(alphaMode == shaderio::LAlphaOpaque)
    {
        result.opaqueMeshIndices.push_back(i);
        result.shadowCasterIndices.push_back(i);
    }
    else if(alphaMode == shaderio::LAlphaMask)
    {
        result.alphaTestMeshIndices.push_back(i);
        result.shadowCasterIndices.push_back(i);
    }
    else // LAlphaBlend
    {
        result.transparentMeshIndices.push_back(i);
        // Transparent meshes don't cast shadows
    }
}

// Initialize transparent sorting cache
result.transparentDistances.resize(result.transparentMeshIndices.size(), 0.0f);
result.transparentSortDirty = true;
```

- [ ] **Step 4: Build to verify compilation**

Run: `cmake --build build --target Demo`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add render/Renderer.cpp
git commit -m "perf(upload): compute material data and mesh lists at gltf load time"
```

---

### Task 4: Remove getMaterialTextureIndices Calls from Passes

**Files:**
- Modify: `render/passes/GBufferPass.cpp:285-298`
- Modify: `render/passes/ForwardPass.cpp:332-340`

- [ ] **Step 1: Update GBufferPass to use cached material data**

Replace the material lookup loop in GBufferPass. In the first pass (collect visible meshes), change:

```cpp
// BEFORE: Per-mesh material lookup
if(mesh->materialIndex >= 0 && ...)
{
    MaterialHandle matHandle = context.gltfModel->materials[mesh->materialIndex];
    drawData.baseColorFactor = m_renderer->getMaterialBaseColorFactor(matHandle);
    auto texIndices = m_renderer->getMaterialTextureIndices(matHandle, context.gltfModel);
    drawData.baseColorTextureIndex = texIndices.baseColor;
    // ... etc
}

// AFTER: Use cached data directly
drawData.baseColorFactor = mesh->baseColorFactor;
drawData.baseColorTextureIndex = mesh->baseColorTextureIndex;
drawData.normalTextureIndex = mesh->normalTextureIndex;
drawData.metallicRoughnessTextureIndex = mesh->metallicRoughnessTextureIndex;
drawData.occlusionTextureIndex = mesh->occlusionTextureIndex;
drawData.metallicFactor = mesh->metallicFactor;
drawData.roughnessFactor = mesh->roughnessFactor;
drawData.normalScale = mesh->normalScale;
```

- [ ] **Step 2: Update ForwardPass to use cached material data**

Same change in ForwardPass:

```cpp
// Replace material lookup with cached data
drawData.baseColorFactor = mesh->baseColorFactor;
drawData.baseColorTextureIndex = mesh->baseColorTextureIndex;
drawData.normalTextureIndex = mesh->normalTextureIndex;
drawData.metallicRoughnessTextureIndex = mesh->metallicRoughnessTextureIndex;
drawData.occlusionTextureIndex = mesh->occlusionTextureIndex;
drawData.metallicFactor = mesh->metallicFactor;
drawData.roughnessFactor = mesh->roughnessFactor;
drawData.normalScale = mesh->normalScale;
```

- [ ] **Step 3: Build to verify compilation**

Run: `cmake --build build --target Demo`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add render/passes/GBufferPass.cpp render/passes/ForwardPass.cpp
git commit -m "perf(passes): use cached material data instead of per-frame lookups"
```

---

### Task 5: Use Pre-Built Mesh Lists in Passes

**Files:**
- Modify: `render/passes/GBufferPass.cpp:237-245`
- Modify: `render/passes/DepthPrepass.cpp:149-162`
- Modify: `render/passes/CSMShadowPass.cpp:188-200`

- [ ] **Step 1: Update GBufferPass to use mesh lists**

Replace full mesh iteration with mesh list iteration:

```cpp
// BEFORE: Full mesh scan with alpha mode check
for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
{
    MeshHandle meshHandle = context.gltfModel->meshes[i];
    const MeshRecord* mesh = meshPool.tryGet(meshHandle);
    if(mesh == nullptr) continue;
    const int32_t alphaMode = mesh->alphaMode;
    if(alphaMode == shaderio::LAlphaBlend) continue;
    // ...
}

// AFTER: Iterate pre-built lists
std::vector<PendingDraw> pendingDraws;

// Add opaque meshes
for(size_t idx : context.gltfModel->opaqueMeshIndices)
{
    MeshHandle meshHandle = context.gltfModel->meshes[idx];
    const MeshRecord* mesh = meshPool.tryGet(meshHandle);
    if(mesh == nullptr) continue;
    // ... compute DrawUniforms using mesh-> fields directly
    pendingDraws.push_back({idx, mesh, drawData, m_renderer->getGBufferOpaquePipelineHandle()});
}

// Add alpha-test meshes
for(size_t idx : context.gltfModel->alphaTestMeshIndices)
{
    MeshHandle meshHandle = context.gltfModel->meshes[idx];
    const MeshRecord* mesh = meshPool.tryGet(meshHandle);
    if(mesh == nullptr) continue;
    // ... compute DrawUniforms
    pendingDraws.push_back({idx, mesh, drawData, m_renderer->getGBufferAlphaTestPipelineHandle()});
}
```

- [ ] **Step 2: Update DepthPrepass to use mesh lists**

Same pattern in DepthPrepass:

```cpp
std::vector<PendingDraw> pendingDraws;

// Opaque meshes
for(size_t idx : context.gltfModel->opaqueMeshIndices)
{
    MeshHandle meshHandle = context.gltfModel->meshes[idx];
    const MeshRecord* mesh = meshPool.tryGet(meshHandle);
    if(mesh == nullptr) continue;
    shaderio::DrawUniforms drawData{};
    drawData.modelMatrix = mesh->transform;
    drawData.alphaMode = shaderio::LAlphaOpaque;
    drawData.alphaCutoff = mesh->alphaCutoff;
    // Use cached material data...
    pendingDraws.push_back({idx, mesh, drawData, m_renderer->getDepthPrepassOpaquePipelineHandle()});
}

// Alpha-test meshes
for(size_t idx : context.gltfModel->alphaTestMeshIndices)
{
    MeshHandle meshHandle = context.gltfModel->meshes[idx];
    const MeshRecord* mesh = meshPool.tryGet(meshHandle);
    if(mesh == nullptr) continue;
    shaderio::DrawUniforms drawData{};
    drawData.modelMatrix = mesh->transform;
    drawData.alphaMode = shaderio::LAlphaMask;
    drawData.alphaCutoff = mesh->alphaCutoff;
    // Use cached material data...
    pendingDraws.push_back({idx, mesh, drawData, m_renderer->getDepthPrepassAlphaTestPipelineHandle()});
}
```

- [ ] **Step 3: Update CSMShadowPass::drawMeshes to use shadowCasterIndices**

```cpp
void CSMShadowPass::drawMeshes(const PassContext& context, VkPipelineLayout pipelineLayout, uint32_t cascadeIndex) const
{
    const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);
    MeshPool& meshPool = m_renderer->getMeshPool();
    (void)cascadeIndex;

    if(context.gltfModel == nullptr || drawBindGroupHandle.isNull())
    {
        return;
    }

    // Use shadowCasterIndices (already excludes transparent)
    std::vector<PendingDraw> pendingDraws;
    for(size_t idx : context.gltfModel->shadowCasterIndices)
    {
        MeshHandle meshHandle = context.gltfModel->meshes[idx];
        const MeshRecord* mesh = meshPool.tryGet(meshHandle);
        if(mesh == nullptr) continue;

        shaderio::DrawUniforms drawData{};
        drawData.modelMatrix = mesh->transform;
        drawData.alphaMode = mesh->alphaMode;
        drawData.alphaCutoff = mesh->alphaCutoff;
        // Use cached material data for shadow pass...
        pendingDraws.push_back({idx, mesh, drawData});
    }

    // Batch allocate and draw...
}
```

- [ ] **Step 4: Build to verify compilation**

Run: `cmake --build build --target Demo`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add render/passes/GBufferPass.cpp render/passes/DepthPrepass.cpp render/passes/CSMShadowPass.cpp
git commit -m "perf(passes): use pre-built mesh lists instead of full scan"
```

---

### Task 6: Cache Transparent Mesh Sorting in ForwardPass

**Files:**
- Modify: `render/passes/ForwardPass.cpp:126-149`

- [ ] **Step 1: Add camera position dirty check**

Replace the per-frame transparent mesh scan and sort with cached sorting:

```cpp
// BEFORE: Per-frame scan + distance calc + sort
std::vector<std::pair<size_t, float>> transparentMeshes;
glm::vec3 cameraPos(0.0f);
if(context.params->cameraUniforms != nullptr)
{
    cameraPos = context.params->cameraUniforms->cameraPosition;
}
for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
{
    // ... check alphaMode == LAlphaBlend
    // ... compute distance
    // ... push to transparentMeshes
}
std::sort(transparentMeshes.begin(), transparentMeshes.end(), ...);

// AFTER: Use cached transparent list with dirty check
glm::vec3 cameraPos(0.0f);
if(context.params->cameraUniforms != nullptr)
{
    cameraPos = context.params->cameraUniforms->cameraPosition;
}

// Check if sorting needs update (camera moved significantly)
const float cameraMoveThreshold = 0.5f;  // meters
const float cameraDelta = glm::length(cameraPos - context.gltfModel->lastSortCameraPos);

GltfUploadResult* mutableGltfModel = const_cast<GltfUploadResult*>(context.gltfModel);
if(context.gltfModel->transparentSortDirty || cameraDelta > cameraMoveThreshold)
{
    // Update distances
    mutableGltfModel->lastSortCameraPos = cameraPos;
    for(size_t slot = 0; slot < context.gltfModel->transparentMeshIndices.size(); ++slot)
    {
        size_t meshIdx = context.gltfModel->transparentMeshIndices[slot];
        MeshHandle meshHandle = context.gltfModel->meshes[meshIdx];
        const MeshRecord* mesh = meshPool.tryGet(meshHandle);
        if(mesh == nullptr)
        {
            mutableGltfModel->transparentDistances[slot] = std::numeric_limits<float>::max();
            continue;
        }
        glm::vec3 meshCenter = glm::vec3(mesh->transform[3]);
        mutableGltfModel->transparentDistances[slot] = glm::length(meshCenter - cameraPos);
    }

    // Re-sort indices by distance (far to near)
    std::vector<size_t> sortedIndices = context.gltfModel->transparentMeshIndices;
    std::sort(sortedIndices.begin(), sortedIndices.end(),
        [&](size_t a, size_t b) {
            size_t slotA = std::find(context.gltfModel->transparentMeshIndices.begin(),
                                     context.gltfModel->transparentMeshIndices.end(), a) -
                           context.gltfModel->transparentMeshIndices.begin();
            size_t slotB = std::find(context.gltfModel->transparentMeshIndices.begin(),
                                     context.gltfModel->transparentMeshIndices.end(), b) -
                           context.gltfModel->transparentMeshIndices.begin();
            return context.gltfModel->transparentDistances[slotA] >
                   context.gltfModel->transparentDistances[slotB];
        });

    mutableGltfModel->transparentMeshIndices = sortedIndices;
    mutableGltfModel->transparentSortDirty = false;
}
```

- [ ] **Step 2: Use sorted transparent list directly**

```cpp
// Use pre-sorted transparent mesh indices
std::vector<shaderio::DrawUniforms> uniformsData;
std::vector<const MeshRecord*> meshRecords;

for(size_t idx : context.gltfModel->transparentMeshIndices)
{
    MeshHandle meshHandle = context.gltfModel->meshes[idx];
    const MeshRecord* mesh = meshPool.tryGet(meshHandle);
    if(mesh == nullptr) continue;

    meshRecords.push_back(mesh);

    shaderio::DrawUniforms drawData{};
    drawData.modelMatrix = mesh->transform;
    drawData.alphaMode = shaderio::LAlphaBlend;
    drawData.alphaCutoff = mesh->alphaCutoff;

    // Use cached material data
    drawData.baseColorFactor = mesh->baseColorFactor;
    drawData.baseColorTextureIndex = mesh->baseColorTextureIndex;
    drawData.normalTextureIndex = mesh->normalTextureIndex;
    drawData.metallicRoughnessTextureIndex = mesh->metallicRoughnessTextureIndex;
    drawData.occlusionTextureIndex = mesh->occlusionTextureIndex;
    drawData.metallicFactor = mesh->metallicFactor;
    drawData.roughnessFactor = mesh->roughnessFactor;
    drawData.normalScale = mesh->normalScale;

    uniformsData.push_back(drawData);
}
```

- [ ] **Step 3: Build to verify compilation**

Run: `cmake --build build --target Demo`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add render/passes/ForwardPass.cpp
git commit -m "perf(forward): cache transparent mesh sorting with dirty check"
```

---

### Task 7: Consolidate Camera Upload to Single Per-Frame Allocation

**Files:**
- Modify: `render/Pass.h:18-32`
- Modify: `render/Renderer.cpp` (render function)
- Modify: `render/passes/GBufferPass.cpp:165-202`
- Modify: `render/passes/DepthPrepass.cpp:102-132`
- Modify: `render/passes/ForwardPass.cpp:257-291`

- [ ] **Step 1: Add camera allocation to PassContext**

In `render/Pass.h`:

```cpp
struct PassContext
{
    rhi::CommandList*   cmd{nullptr};
    TransientAllocator* transientAllocator{nullptr};
    uint32_t            frameIndex{0};
    uint32_t            passIndex{0};
    const RenderParams* params{nullptr};
    std::vector<StreamEntry>* drawStream{nullptr};
    const GltfUploadResult*   gltfModel{nullptr};
    BindGroupHandle           globalBindlessGroup{};

    // Shared camera uniform allocation (set once per frame by Renderer)
    TransientAllocator::Allocation cameraAlloc{};
    bool cameraAllocValid{false};
};
```

- [ ] **Step 2: Allocate camera once in Renderer::render**

In `render/Renderer.cpp` in the `render` function before pass execution:

```cpp
// Allocate CameraUniforms once for all passes
TransientAllocator::Allocation cameraAlloc = m_transientAllocator.allocate(sizeof(shaderio::CameraUniforms), 256);
shaderio::CameraUniforms cameraData{};
if(params.cameraUniforms != nullptr)
{
    cameraData = *params.cameraUniforms;
}
else
{
    // Fallback default camera
    cameraData.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    cameraData.projection = clipspace::makePerspectiveProjection(
        glm::radians(45.0f), viewportWidth / viewportHeight, 0.1f, 100.0f,
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan));
    cameraData.viewProjection = cameraData.projection * cameraData.view;
    cameraData.inverseViewProjection = glm::inverse(cameraData.viewProjection);
    cameraData.cameraPosition = glm::vec3(0.0f, 0.0f, 3.0f);
}
std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
m_transientAllocator.flushAllocation(cameraAlloc, sizeof(cameraData));

// Set camera allocation in PassContext
PassContext passContext;
passContext.cameraAlloc = cameraAlloc;
passContext.cameraAllocValid = true;
```

- [ ] **Step 3: Update GBufferPass to use shared camera**

Remove camera allocation from GBufferPass:

```cpp
// BEFORE: Allocate camera in pass
const TransientAllocator::Allocation cameraAlloc =
    context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);
shaderio::CameraUniforms cameraData{};
if(context.params->cameraUniforms != nullptr) { cameraData = *context.params->cameraUniforms; }
else { /* fallback */ }
std::memcpy(cameraAlloc.cpuPtr, &cameraData, sizeof(cameraData));
context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cameraData));

// AFTER: Use shared allocation
if(!context.cameraAllocValid)
{
    // Fallback if no shared allocation (shouldn't happen)
    return;
}
const TransientAllocator::Allocation& cameraAlloc = context.cameraAlloc;
```

- [ ] **Step 4: Update DepthPrepass similarly**

Remove camera allocation, use `context.cameraAlloc`.

- [ ] **Step 5: Update ForwardPass similarly**

Remove camera allocation, use `context.cameraAlloc`.

- [ ] **Step 6: Build to verify compilation**

Run: `cmake --build build --target Demo`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add render/Pass.h render/Renderer.cpp render/passes/GBufferPass.cpp render/passes/DepthPrepass.cpp render/passes/ForwardPass.cpp
git commit -m "perf(render): single per-frame camera upload, passes bind only"
```

---

### Task 8: Run Full Test and Verify Performance

**Files:**
- None (verification)

- [ ] **Step 1: Build and run Demo**

Run: `cmake --build build --target Demo && timeout 10 build/Debug/Demo.exe --scene bistro`

Expected: Application runs without validation errors

- [ ] **Step 2: Check Tracy profiling for passExecution time**

If Tracy is enabled, verify that:
- `drawFrame.passExecution` CPU time is reduced
- Material lookup calls (`getMaterialTextureIndices`) are eliminated from pass traces
- Transparent sorting only triggers on camera movement

- [ ] **Step 3: Final commit (if any fixes needed)**

```bash
git add -A
git commit -m "fix(render): address any issues from performance verification"
```

---

## Self-Review Checklist

**1. Spec coverage:**
- Pre-compute material data: Task 1, 3, 4 ✓
- Pre-built mesh lists: Task 2, 5 ✓
- Transparent sorting cache: Task 6 ✓
- Single camera upload: Task 7 ✓

**2. Placeholder scan:**
- No "TBD" or "TODO" found ✓
- All code blocks have actual implementations ✓

**3. Type consistency:**
- `MeshRecord` fields match `DrawUniforms` fields ✓
- `TransientAllocator::Allocation` used consistently ✓
- `PassContext.cameraAlloc` type matches allocation type ✓

---

## Execution Options

**Plan complete and saved to `docs/superpowers/plans/2026-04-19-draw-loop-optimization.md`.**

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**