# Bistro Scene Loading Optimization Design

**Date:** 2026-05-01
**Project:** VKDemo
**Author:** Claude (brainstorming session with user)

---

## Overview

This design specifies optimizations to reduce Bistro scene loading time from **3 minutes to under 10 seconds**. The optimization is implemented in four phases, each providing incremental improvement.

### Current Bottleneck Analysis

| Stage | Current Implementation | Time | Bottleneck |
|-------|----------------------|------|------------|
| **File Parsing** | tinygltf single-threaded | 15-30 sec | CPU parsing |
| **Texture Upload** | Per-texture staging, uncompressed R8G8B8A8 | 90-120 sec | 200MB data, 600 allocations |
| **Mesh Upload** | Per-mesh buffers, CPU interleaving | 30-60 sec | 500+ small transfers |
| **Sync Wait** | vkDeviceWaitIdle each batch | 5-10 sec | Blocking GPU |
| **Total** | | **~180 sec** | |

### Target

| Metric | Current | Target |
|--------|---------|--------|
| First load (cold) | 180 sec | < 10 sec |
| Cached load | N/A | < 3 sec |
| Visible scene | 180 sec | < 2 sec (progressive) |
| Texture data | 200 MB | 50 MB (compressed) |

---

## Implementation Approach

**Approach A: Phased Implementation** - Each phase provides measurable improvement, tested independently.

| Phase | Strategy | Expected Impact | Time |
|-------|----------|----------------|------|
| Phase 1 | Batch Staging Buffers | 180s → 60s | ~1 week |
| Phase 2 | Texture Compression + Mipmaps | 60s → 15s | ~2 weeks |
| Phase 3 | Scene Cache Serialization | 15s → 3s (cached) | ~1 week |
| Phase 4 | Async Loading Pipeline | Visible in 2s | ~2 weeks |

**Total: ~6 weeks**

---

## Phase 1: Batch Staging Buffers

### Goal

Combine all mesh/texture uploads into single large staging buffer. Eliminate per-asset allocation overhead.

### Core Component: BatchUploadContext

```cpp
class BatchUploadContext {
public:
    void init(VmaAllocator allocator, size_t totalSize);
    
    struct Slice {
        void* cpuPtr;        // mapped pointer for CPU write
        size_t offset;       // offset in staging buffer
        size_t size;         // slice size
    };
    Slice allocate(size_t size, size_t alignment);
    
    void recordTextureUpload(const Slice& slice, VkImage dstImage, 
                             const VkBufferImageCopy& region);
    void recordBufferUpload(const Slice& slice, VkBuffer dstBuffer,
                            const VkBufferCopy& region);
    
    void executeUploads(VkDevice device, VkQueue queue);
    void destroy();

private:
    VkBuffer m_stagingBuffer;
    VmaAllocation m_stagingAllocation;
    void* m_mappedData;
    size_t m_capacity;
    size_t m_head;
    
    std::vector<UploadOperation> m_pendingUploads;
};
```

### Modified Loading Pipeline

```cpp
GltfUploadResult uploadGltfModel(const GltfModel& model) {
    // 1. Pre-calculate total staging size
    size_t totalSize = 0;
    for (const auto& img : model.images) 
        totalSize += img.pixels.size();
    for (const auto& mesh : model.meshes) {
        totalSize += mesh.positions.size() / 3 * 48;  // interleaved
        totalSize += mesh.indices.size() * 4;
    }
    
    // 2. Create single batch staging buffer
    BatchUploadContext batch;
    batch.init(m_allocator, totalSize);
    
    // 3. Copy all data to staging (CPU-side, parallelizable)
    for (const auto& img : model.images) {
        Slice slice = batch.allocate(img.pixels.size(), 1);
        memcpy(slice.cpuPtr, img.pixels.data(), img.pixels.size());
        batch.recordTextureUpload(slice, ...);
    }
    
    for (const auto& mesh : model.meshes) {
        Slice vSlice = batch.allocate(vertexSize, 1);
        buildInterleavedVertices(vSlice.cpuPtr, mesh);  // CPU interleave
        batch.recordBufferUpload(vSlice, vertexBuffer);
        
        Slice iSlice = batch.allocate(indexSize, 1);
        memcpy(iSlice.cpuPtr, mesh.indices.data(), indexSize);
        batch.recordBufferUpload(iSlice, indexBuffer);
    }
    
    // 4. Single GPU transfer
    batch.executeUploads(m_device, m_transferQueue);
    
    return result;
}
```

### Expected Results

| Metric | Before | After |
|--------|--------|-------|
| Staging allocations | 600+ | 1 |
| GPU transfers | 600+ | 1 |
| vkDeviceWaitIdle calls | 600+ | 1 |
| Load time | 180 sec | ~60 sec |

---

## Phase 2: Texture Compression + Mipmap Generation

### Goal

Reduce texture data by ~75% using BC7 compression. Generate mipmaps for quality and virtual texture foundation.

### 2A: Texture Compression (BC7/KTX2)

**Offline conversion pipeline**:

```
PNG/JPEG (source) → KTX2 (BC7 compressed)

Conversion tool: tools/texture_converter.cpp

Input:  Bistro/glTF/textures/*.png
Output: Bistro/ktx2/texture_*.ktx2

Size comparison:
• PNG 1024x1024 RGBA: ~2 MB
• KTX2 BC7 1024x1024 + mipmaps: ~682 KB
• Reduction: ~66%
```

**KTX2 loader**:

```cpp
class Ktx2Loader {
public:
    struct Ktx2Texture {
        VkFormat format;           // VK_FORMAT_BC7_UNORM_BLOCK
        uint32_t width;
        uint32_t height;
        uint32_t mipLevels;
        std::vector<size_t> mipOffsets;
        std::vector<size_t> mipSizes;
        std::vector<uint8_t> data;
    };
    
    bool load(const std::string& filepath, Ktx2Texture& out);
};
```

**GPU upload (direct)**:

```cpp
// BC7 is block-compressed - upload directly without CPU decode
void uploadCompressedTexture(VkCommandBuffer cmd, 
                              const Ktx2Texture& ktx,
                              VkImage dstImage) {
    for (uint32_t mip = 0; mip < ktx.mipLevels; ++mip) {
        VkBufferImageCopy copy = {
            .bufferOffset = ktx.mipOffsets[mip],
            .imageSubresource = {.mipLevel = mip, .layerCount = 1},
            .imageExtent = {ktx.width >> mip, ktx.height >> mip, 1}
        };
        vkCmdCopyBufferToImage(cmd, stagingBuffer, dstImage, 
                               VK_IMAGE_LAYOUT_TRANSFER_DST, 1, &copy);
    }
}
```

### 2B: Mipmap Generation (GPU)

```cpp
class MipmapGenerator {
public:
    void generateMipmaps(VkCommandBuffer cmd, VkImage image,
                         uint32_t baseWidth, uint32_t baseHeight,
                         uint32_t mipLevels);
};

// Iterative blit from mip N to N+1
void MipmapGenerator::generateMipmaps(...) {
    for (uint32_t mip = 0; mip < mipLevels - 1; ++mip) {
        VkImageBlit blit = {
            .srcSubresource = {.mipLevel = mip},
            .srcOffsets = {{0,0,0}, {width>>mip, height>>mip, 1}},
            .dstSubresource = {.mipLevel = mip + 1},
            .dstOffsets = {{0,0,0}, {width>>(mip+1), height>>(mip+1), 1}}
        };
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST,
                       1, &blit, VK_FILTER_LINEAR);
    }
}
```

### Expected Results

| Metric | Before | After |
|--------|--------|-------|
| Texture data size | 200 MB | 50-68 MB |
| Upload time | 90-120 sec | 15-25 sec |
| GPU memory | 200 MB | 50 MB |
| Rendering quality | No mipmaps | Full mip chain |

---

## Phase 3: Scene Cache Serialization

### Goal

Save processed scene to binary cache. Skip glTF parsing and CPU processing on subsequent loads.

### Binary Cache Format (.vkcache)

```
Header (64 bytes):
  • magic: "VKCACHE"
  • version: uint32
  • sourceTimestamp: uint64
  • sourcePathHash: uint64
  • meshCount, materialCount, textureCount
  • totalVertexDataSize, totalIndexDataSize

Texture Section:
  • TextureCacheEntry[textureCount]
    - format, width, height, mipLevels
    - ktxPathOffset (reference to KTX2)
  
Material Section:
  • MaterialCacheEntry[materialCount]
    - baseColorFactor, metallicFactor, roughnessFactor
    - normalScale, occlusionStrength, emissiveFactor
    - alphaMode, alphaCutoff
    - textureIndices[5]
  
Mesh Section:
  • MeshCacheEntry[meshCount]
    - vertexCount, indexCount
    - vertexDataOffset, indexDataOffset
    - transform, boundsMin, boundsMax
    - materialIndex, alphaMode
  • VertexData (pre-interleaved, 48 bytes/vertex)
  • IndexData (uint32)
```

### SceneCacheSerializer Class

```cpp
class SceneCacheSerializer {
public:
    bool saveCache(const std::string& cachePath,
                   const GltfModel& model,
                   const CacheMetadata& metadata);
    
    bool loadCache(const std::string& cachePath,
                   GltfModel& model,
                   CacheMetadata& outMetadata);
    
    bool isCacheValid(const std::string& cachePath,
                      const std::string& sourcePath);
};
```

### Modified uploadGltfModel

```cpp
GltfUploadResult uploadGltfModel(const std::string& gltfPath) {
    std::string cachePath = getCachePath(gltfPath);
    
    if (m_cache.isCacheValid(cachePath, gltfPath)) {
        // Fast path: load from cache
        GltfModel cachedModel;
        m_cache.loadCache(cachePath, cachedModel);
        return uploadFromCache(cachedModel);  // Pre-interleaved!
    }
    
    // Slow path: parse glTF
    GltfModel model;
    GltfLoader::load(gltfPath, model);
    
    GltfUploadResult result = uploadFromGltf(model);
    
    // Save cache
    m_cache.saveCache(cachePath, model, getMetadata(gltfPath));
    
    return result;
}
```

### Expected Results

| Metric | First Load | Cached Load |
|--------|------------|-------------|
| File parsing | 15-30 sec | 0 sec |
| Vertex interleaving | 5-10 sec | 0 sec |
| Tangent generation | 10-20 sec | 0 sec |
| Total | ~15-25 sec | ~3-5 sec |

---

## Phase 4: Async Loading Pipeline

### Goal

Parallelize CPU parsing and GPU uploads. Progressive rendering with visible scene in ~2 seconds.

### Architecture

```
Load Thread (CPU)     Upload Queue (GPU)      Render Thread (Main)
      │                     │                       │
      │ Parse glTF          │ Async copy            │ Progressive
      │ Build staging       │ to GPU                │ rendering
      │ Submit requests     │ Track completion      │
      │                     │                       │ Visible in 2s
```

### Core Components

```cpp
class UploadQueue {
public:
    struct UploadRequest {
        enum Type { Texture, Mesh, Complete };
        Type type;
        uint32_t priority;      // Higher = load first
        std::promise<void> completionPromise;
    };
    
    void init(VkDevice device, VkQueue transferQueue);
    void submit(UploadRequest request);
    void processNext();
};

class AsyncLoadingCoordinator {
public:
    struct LoadProgress {
        uint32_t texturesLoaded, texturesTotal;
        uint32_t meshesLoaded, meshesTotal;
        float progressPercent;
        bool isComplete;
    };
    
    std::future<GltfUploadResult> loadAsync(const std::string& path,
                                             const glm::vec3& cameraPos);
    LoadProgress getProgress() const;
    GltfUploadResult getVisibleAssets() const;
};
```

### Asset Priority System

```cpp
enum AssetPriority : uint32_t {
    Critical   = 100,   // Camera-visible, essential
    High       = 50,    // Near camera
    Medium     = 25,    // Mid-distance
    Low        = 10,    // Far, background
    Background = 1,     // Not visible initially
};

uint32_t calculatePriority(const glm::vec3& meshCenter,
                           const glm::vec3& cameraPos) {
    float distance = glm::length(meshCenter - cameraPos);
    if (distance < 10.0f)  return Critical;
    if (distance < 25.0f)  return High;
    if (distance < 50.0f)  return Medium;
    if (distance < 100.0f) return Low;
    return Background;
}
```

### Progressive Rendering

```cpp
void Renderer::onAssetsReady(const GltfUploadResult& partial) {
    std::lock_guard<std::mutex> lock(m_sceneMutex);
    
    for (MeshHandle mesh : partial.meshes) {
        addToDrawList(mesh);
    }
    
    requestRedraw();
}

void Renderer::render() {
    std::lock_guard<std::mutex> lock(m_sceneMutex);
    
    // Draw only loaded assets
    for (MeshHandle mesh : m_drawList) {
        if (m_resourceSync.isReady(mesh)) {
            drawMesh(mesh);
        } else {
            drawPlaceholder(getBounds(mesh));  // Bounding box wireframe
        }
    }
    
    if (m_loader.isLoading()) {
        drawProgressOverlay(m_loader.getProgress());
    }
}
```

### Transfer Queue

```cpp
VkQueue getTransferQueue() {
    // Prefer dedicated transfer queue (async)
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (props[i].queueFlags & VK_QUEUE_TRANSFER_BIT &&
            !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            return getQueue(i, 0);  // Dedicated transfer
        }
    }
    // Fallback: graphics queue (shared)
    return m_graphicsQueue;
}
```

### Expected Results

| Metric | Before | After |
|--------|--------|-------|
| Visible scene | After complete | ~2 seconds |
| Full detail | 10 sec | 10 sec |
| User wait | Blocking | Interactive in 2s |
| CPU utilization | Single thread | Parallel |

**Timeline**:
```
T+0s:   Loading starts
T+2s:   Critical assets visible (near camera)
T+5s:   Most assets loaded
T+10s:  Complete
```

---

## New Files to Create

```
render/BatchUploadContext.h/.cpp         - Phase 1
loader/Ktx2Loader.h/.cpp                 - Phase 2
render/MipmapGenerator.h/.cpp            - Phase 2
loader/SceneCacheSerializer.h/.cpp       - Phase 3
render/UploadQueue.h/.cpp                 - Phase 4
render/AsyncLoadingCoordinator.h/.cpp     - Phase 4

tools/texture_converter.cpp               - Offline BC7 conversion
```

---

## Testing Strategy

Each phase tested independently:

**Phase 1 Tests**:
- Verify single staging buffer allocation
- Measure load time reduction
- Compare memory allocation count

**Phase 2 Tests**:
- Verify KTX2 loading
- Check BC7 format correctness
- Verify mipmap generation quality

**Phase 3 Tests**:
- Verify cache serialization
- Test cache invalidation (source change)
- Measure cached load time

**Phase 4 Tests**:
- Verify progressive rendering
- Test asset priority ordering
- Measure visible scene timing

**Performance Benchmarks**:
- Bistro scene load time (before/after each phase)
- Memory allocation count
- GPU utilization during load

---

## Integration with Existing Code

| Existing Component | Optimization Integration |
|-------------------|------------------------|
| `Renderer::uploadGltfModel` | Use BatchUploadContext + cache check |
| `GltfLoader` | Keep for fallback, add Ktx2Loader |
| `MeshPool::uploadMesh` | Receive pre-interleaved data from cache |
| `Renderer::render` | Progressive rendering + progress overlay |

---

## Requirements

- Vulkan transfer queue support (most GPUs)
- BC7 texture format support (DX11 level)
- Multi-threading support
- File system for cache storage

---

## Success Criteria

1. Bistro first load: < 10 seconds
2. Bistro cached load: < 3 seconds
3. Visible scene: < 2 seconds (progressive)
4. Texture memory: < 50% of original
5. No rendering regressions
6. Progress UI visible during loading

---

## Timeline Summary

| Phase | Duration | Cumulative Load Time |
|-------|----------|---------------------|
| Phase 1 | 1 week | ~60 sec |
| Phase 2 | 2 weeks | ~15 sec |
| Phase 3 | 1 week | ~3 sec cached |
| Phase 4 | 2 weeks | ~2 sec visible |
| **Total** | **6 weeks** | **<10 sec first, <3 sec cached** |