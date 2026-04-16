# RHI Pass Layer Full Encapsulation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans.

**Goal:** Make Pass layer use only RHI interfaces, no native Vulkan types.

**Architecture:** Unify handle types, extend TextureViewHandle to support native handles, update MeshRecord to use RHI buffer handles.

**Tech Stack:** C++20, Vulkan 1.3

---

## Problem Analysis

### Current Issues

1. **Two Handle Systems**:
   - `demo::BindGroupHandle` (common/Handles.h)
   - `demo::rhi::BindGroupHandle` (rhi/RHIHandles.h)
   - Pass uses demo:: but RHI expects rhi::

2. **TextureViewHandle Can't Store Native Pointers**:
   - 32-bit index + 32-bit generation
   - Can't store 64-bit VkImageView

3. **MeshRecord Uses Native Vulkan**:
   - `VkBuffer vertexBuffer`, `VkBuffer indexBuffer`
   - Not `rhi::BufferHandle`

### Solution Approach

1. **Unify Handle Types**: Make `demo::BindGroupHandle` alias to `demo::rhi::BindGroupHandle`
2. **TextureViewHandle**: Add methods to encode/decode 64-bit native pointers
3. **MeshRecord**: Use opaque buffer handles instead of VkBuffer directly

---

## Phase 1: Unify Handle Types

### Task 1: Make demo handles alias to rhi handles

**Files:**
- Modify: `common/Handles.h`

**Approach:**
1. Include `rhi/RHIHandles.h` in `common/Handles.h`
2. Make `demo::BufferHandle` = `demo::rhi::BufferHandle`
3. Make `demo::TextureHandle` = `demo::rhi::TextureHandle`
4. Make `demo::PipelineHandle` = `demo::rhi::PipelineHandle`
5. Make `demo::BindGroupHandle` = `demo::rhi::BindGroupHandle`

```cpp
// common/Handles.h
#pragma once

#include "../rhi/RHIHandles.h"

namespace demo {

// Use RHI handles directly
using BufferHandle   = rhi::BufferHandle;
using TextureHandle  = rhi::TextureHandle;
using PipelineHandle = rhi::PipelineHandle;
using SamplerHandle  = rhi::SamplerHandle;
using BindGroupHandle = rhi::BindGroupHandle;

// Keep application-specific handles
DEMO_DECLARE_TYPED_HANDLE(MeshHandle);
DEMO_DECLARE_TYPED_HANDLE(MaterialHandle);

}  // namespace demo
```

---

## Phase 2: TextureViewHandle Native Pointer Support

### Task 2: Add native pointer encoding to TextureViewHandle

**Files:**
- Modify: `rhi/RHIHandles.h`

**Approach:** Add helper functions to encode/decode 64-bit pointers in the 64-bit handle space:

```cpp
// In rhi/RHIHandles.h

// Extend TextureViewHandle with native pointer support
struct TextureViewHandle : Handle<TextureViewTag>
{
  // Create from native pointer (encodes 64-bit pointer into index+generation)
  static TextureViewHandle fromNativePtr(void* nativePtr) {
    const uint64_t value = reinterpret_cast<uint64_t>(nativePtr);
    TextureViewHandle h;
    h.index = static_cast<uint32_t>(value & 0xFFFFFFFF);
    h.generation = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
    return h;
  }
  
  // Get native pointer from handle
  void* toNativePtr() const {
    const uint64_t value = (static_cast<uint64_t>(generation) << 32) | index;
    return reinterpret_cast<void*>(value);
  }
};
```

---

## Phase 3: MeshRecord RHI Handles

### Task 3: Update MeshRecord to use opaque buffer handles

**Files:**
- Modify: `render/MeshPool.h`
- Modify: `render/MeshPool.cpp`

**Approach:**
1. Store opaque 64-bit handles instead of VkBuffer
2. MeshPool internally converts to VkBuffer when needed

```cpp
// render/MeshPool.h
struct MeshRecord {
    uint64_t vertexBufferHandle = 0;  // Opaque buffer handle
    uint64_t indexBufferHandle = 0;   // Opaque buffer handle
    VmaAllocation vertexAllocation = nullptr;
    VmaAllocation indexAllocation = nullptr;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 48;
    glm::mat4 transform = glm::mat4(1.0f);
    int32_t materialIndex = -1;
};
```

---

## Phase 4: CommandList Buffer Binding

### Task 4: Update CommandList::bindVertexBuffers/bindIndexBuffer

**Files:**
- Modify: `rhi/RHICommandList.h`
- Modify: `rhi/vulkan/VulkanCommandList.cpp`

**Approach:** Accept uint64_t opaque handles that can be reinterpret as native buffers:

```cpp
// RHICommandList.h
virtual void bindVertexBuffers(uint32_t firstBinding, const uint64_t* bufferHandles, 
                               const uint64_t* offsets, uint32_t bufferCount) = 0;
virtual void bindIndexBuffer(uint64_t bufferHandle, uint64_t offset, IndexFormat format) = 0;
```

---

## Phase 5: Update Passes to Use RHI

### Task 5: Update GBufferPass to use RHI interfaces fully

**Files:**
- Modify: `render/passes/GBufferPass.cpp`

**Approach:**
1. Use `cmd->beginRenderPass(renderPassDesc)` with TextureViewHandle from native pointers
2. Use `cmd->bindPipeline()` with unified handle
3. Use `cmd->bindBindGroup()` with unified handle
4. Use `cmd->bindVertexBuffers()` with opaque handles
5. Use `cmd->bindIndexBuffer()` with opaque handle
6. Use `cmd->drawIndexed()`

### Task 6: Update ForwardPass similarly

### Task 7: Update LightPass similarly

---

## Summary

| Phase | Tasks | Key Changes |
|-------|-------|-------------|
| 1 | 1 | Unify demo:: and rhi:: handle types |
| 2 | 2 | TextureViewHandle native pointer encoding |
| 3 | 3 | MeshRecord uses opaque buffer handles |
| 4 | 4 | CommandList accepts opaque buffer handles |
| 5 | 5-7 | Passes use RHI interfaces fully |

Total: 7 tasks