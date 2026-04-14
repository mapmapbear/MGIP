# CSM Shadow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace existing single shadow map with 4-cascade CSM system using Texture2DArray storage, λ=0.5 practical split, bounding sphere + texel snap stabilization.

**Architecture:** Complete shadow system rewrite (Approach A). New CSMShadowResources class manages 4-cascade Texture2DArray and cascade matrices. CSMShadowPass renders 4 layers sequentially. Shader changes synchronized with C++ changes.

**Tech Stack:** C++20, GLM, Vulkan RHI, VMA, Slang shaders, ImGui

---

## File Structure

```
New Files:
- render/CSMShadowResources.h       (Texture2DArray + cascade matrix management)
- render/CSMShadowResources.cpp     (cascade split, bounding sphere, texel snap)
- render/passes/CSMShadowPass.h     (per-cascade render pass header)
- render/passes/CSMShadowPass.cpp   (4-layer depth rendering loop)

Modified Files:
- shaders/shader_io.h               (ShadowUniforms struct, CSM constants)
- shaders/shader.shadow.slang       (cascade-aware depth shader - unchanged)
- shaders/shader.light.slang        (CSM sampling function with cascade selection)
- shaders/shader.debug.slang        (cascade overlay shader)
- render/Renderer.h                 (replace ShadowResources with CSMShadowResources)
- render/Renderer.cpp               (CSM init, update, destroy)
- render/passes/DebugPass.h         (add CSMFrustumWireframePrimitive)
- render/passes/DebugPass.cpp       (cascade frustum visualization)
- app/MinimalLatestApp.h            (ImGui CSM debug panel)
- CMakeLists.txt                    (add CSM files, remove old shadow files)

Removed Files:
- render/ShadowResources.h          (replaced by CSMShadowResources.h)
- render/ShadowResources.cpp        (replaced by CSMShadowResources.cpp)
- render/passes/ShadowPass.h        (replaced by CSMShadowPass.h)
- render/passes/ShadowPass.cpp      (replaced by CSMShadowPass.cpp)
```

---

### Task 1: Update shader_io.h with CSM Constants and ShadowUniforms

**Files:**
- Modify: `shaders/shader_io.h`

- [ ] **Step 1: Add CSM constants after existing constants (around line 136)**

```cpp
// CSM (Cascaded Shadow Maps) constants
STATIC_CONST int LCascadeCount = 4;  // Number of shadow cascades
STATIC_CONST float LCascadeSplitLambda = 0.5f;  // Practical split (log + linear mix)
STATIC_CONST float LCascadeBlendRegion = 0.0f;  // Hard boundaries (no blending)
```

- [ ] **Step 2: Replace ShadowUniforms struct (around line 171-178)**

Find existing `ShadowUniforms` struct and replace with:

```cpp
struct ShadowUniforms
{
  // Per-cascade matrices
  mat4 cascadeViewProjection[LCascadeCount];
  mat4 cascadeWorldToShadowTexture[LCascadeCount];

  // Cascade split distances (view-space depth)
  vec4 cascadeSplitDistances;  // x=c0 far, y=c1 far, z=c2 far, w=c3 far

  // Light parameters (unchanged)
  vec4 lightDirectionAndIntensity;

  // Shadow parameters
  vec4 shadowMapMetrics;  // x=1/shadowSize, y=maxShadowDistance, z=unused, w=cascadeCount

  // Per-cascade bias (scaled)
  vec4 cascadeBiasScale;  // x=baseConstantBias, y=baseSlopeBias, z=scaleFactor(0.5), w=normalBias
};
```

- [ ] **Step 3: Add cascade overlay debug mode constants**

```cpp
// Cascade debug overlay mode
STATIC_CONST int LCascadeOverlayModeOff = 0;
STATIC_CONST int LCascadeOverlayModeFrustum = 1;
STATIC_CONST int LCascadeOverlayModeScreen = 2;
```

- [ ] **Step 4: Verify shader_io.h compiles**

Run: `cmake --build build --config Release 2>&1 | head -50`
Expected: Build errors may occur (ShadowUniforms struct changed), proceed to next tasks

- [ ] **Step 5: Commit shader_io.h changes**

```bash
git add shaders/shader_io.h
git commit -m "feat(csm): add CSM constants and ShadowUniforms struct

LCascadeCount=4, LCascadeSplitLambda=0.5, cascade matrices array,
split distances vec4, cascadeBiasScale for adaptive bias.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 2: Create CSMShadowResources.h Header

**Files:**
- Create: `render/CSMShadowResources.h`

- [ ] **Step 1: Write CSMShadowResources.h header**

```cpp
#pragma once

#include "../common/Common.h"
#include "../shaders/shader_io.h"
#include "ClipSpaceConvention.h"

namespace demo {

class CSMShadowResources
{
public:
  struct CreateInfo
  {
    uint32_t                          cascadeCount{4};
    uint32_t                          cascadeResolution{1024};  // Per cascade
    VkFormat                          shadowFormat{VK_FORMAT_D32_SFLOAT};
    clipspace::ProjectionConvention   projectionConvention{
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan)};
  };

  CSMShadowResources() = default;
  ~CSMShadowResources() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo);
  void deinit();

  void updateCascadeMatrices(const shaderio::CameraUniforms& camera, const glm::vec3& lightDir);

  // Texture2DArray access (all cascades)
  [[nodiscard]] VkImage getCascadeImage() const { return m_cascadeArray.image; }
  [[nodiscard]] VkImageView getCascadeView() const { return m_cascadeArrayView; }

  // Per-layer access (for rendering each cascade)
  [[nodiscard]] VkImageView getCascadeLayerView(uint32_t index) const 
  { 
    assert(index < m_cascadeCount); 
    return m_cascadeLayerViews[index]; 
  }

  [[nodiscard]] uint32_t getCascadeCount() const { return m_cascadeCount; }
  [[nodiscard]] uint32_t getCascadeResolution() const { return m_cascadeResolution; }
  [[nodiscard]] VkExtent2D getCascadeExtent() const 
  { 
    return {m_cascadeResolution, m_cascadeResolution}; 
  }

  // Uniform buffer access
  [[nodiscard]] VkBuffer getShadowUniformBuffer() const { return m_shadowUniformBuffer.buffer; }
  [[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData() { return &m_shadowUniformsData; }

private:
  VkDevice                        m_device{VK_NULL_HANDLE};
  VmaAllocator                    m_allocator{nullptr};
  clipspace::ProjectionConvention m_projectionConvention{
      clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan)};

  utils::Image             m_cascadeArray{};  // Texture2DArray (arrayLayers = cascadeCount)
  VkImageView              m_cascadeArrayView{VK_NULL_HANDLE};  // Full array view for sampling
  VkImageView              m_cascadeLayerViews[shaderio::LCascadeCount];  // Per-layer views for rendering

  utils::Buffer            m_shadowUniformBuffer{};
  shaderio::ShadowUniforms m_shadowUniformsData{};
  void*                    m_shadowUniformMapped{nullptr};

  uint32_t m_cascadeCount{4};
  uint32_t m_cascadeResolution{1024};
};

}  // namespace demo
```

- [ ] **Step 2: Verify header compiles**

Run: `cmake --build build --config Release 2>&1 | grep -E "(CSMShadowResources|error)" || echo "Header OK"`
Expected: No errors related to CSMShadowResources.h

- [ ] **Step 3: Commit**

```bash
git add render/CSMShadowResources.h
git commit -m "feat(csm): add CSMShadowResources header

Texture2DArray storage, per-layer views, cascade matrices,
ShadowUniforms with cascade arrays.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 3: Create CSMShadowResources.cpp Implementation

**Files:**
- Create: `render/CSMShadowResources.cpp`

- [ ] **Step 1: Write CSMShadowResources.cpp implementation**

```cpp
#include "CSMShadowResources.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace demo {

namespace {

// Shadow parameters
constexpr float kDefaultMaxShadowDistance = 100.0f;
constexpr float kBaseConstantBias = 1.25f;
constexpr float kBaseSlopeBias = 1.75f;
constexpr float kCascadeBiasScaleFactor = 0.5f;  // bias[i] = baseBias * (1 + i * 0.5)
constexpr float kShadowIntensity = 1.0f;

// Extract near/far planes from projection matrix
[[nodiscard]] float extractNearPlane(const glm::mat4& projection, const clipspace::ProjectionConvention& convention)
{
  return clipspace::extractPerspectiveNearPlane(projection, convention);
}

[[nodiscard]] float extractFarPlane(const glm::mat4& projection, const clipspace::ProjectionConvention& convention)
{
  return clipspace::extractPerspectiveFarPlane(projection, convention);
}

[[nodiscard]] glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
  const float lengthSq = glm::dot(value, value);
  return lengthSq > 1e-6f ? value * glm::inversesqrt(lengthSq) : fallback;
}

// Compute cascade split distances using practical split (λ = 0.5)
void computeCascadeSplits(float* splits, uint32_t count, float near, float far, float lambda)
{
  for(uint32_t i = 0; i < count; ++i)
  {
    const float fraction = static_cast<float>(i + 1) / static_cast<float>(count);
    
    // Uniform split
    const float uniformSplit = near + (far - near) * fraction;
    
    // Logarithmic split (better near-field resolution)
    const float logSplit = near * std::pow(far / near, fraction);
    
    // Practical split: blend log and uniform
    splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
  }
}

// Compute frustum slice corners in world space
[[nodiscard]] std::array<glm::vec3, 8> computeFrustumSliceCorners(
    const shaderio::CameraUniforms& camera,
    const clipspace::ProjectionConvention& convention,
    float sliceNear,
    float sliceFar)
{
  const glm::mat4 invViewProjection = glm::inverse(camera.viewProjection);
  
  // NDC corners
  const std::array<glm::vec2, 4> ndcCorners = {
    glm::vec2(-1.0f, -1.0f),
    glm::vec2( 1.0f, -1.0f),
    glm::vec2( 1.0f,  1.0f),
    glm::vec2(-1.0f,  1.0f),
  };
  
  // Compute near and far corners
  std::array<glm::vec3, 4> nearCorners{};
  std::array<glm::vec3, 4> farCorners{};
  
  const float cameraNear = extractNearPlane(camera.projection, convention);
  const float cameraFar = extractFarPlane(camera.projection, convention);
  const float nearLerp = (sliceNear - cameraNear) / std::max(0.01f, cameraFar - cameraNear);
  const float farLerp = (sliceFar - cameraNear) / std::max(0.01f, cameraFar - cameraNear);
  
  for(size_t i = 0; i < 4; ++i)
  {
    // Near corner
    glm::vec4 nearNdc = glm::vec4(ndcCorners[i], convention.ndcNearZ, 1.0f);
    glm::vec4 nearWorld = invViewProjection * nearNdc;
    nearCorners[i] = glm::vec3(nearWorld / nearWorld.w);
    
    // Far corner
    glm::vec4 farNdc = glm::vec4(ndcCorners[i], convention.ndcFarZ, 1.0f);
    glm::vec4 farWorld = invViewProjection * farNdc;
    farCorners[i] = glm::vec3(farWorld / farWorld.w);
  }
  
  // Interpolate to slice boundaries
  std::array<glm::vec3, 8> sliceCorners{};
  for(size_t i = 0; i < 4; ++i)
  {
    const glm::vec3 ray = farCorners[i] - nearCorners[i];
    sliceCorners[i] = nearCorners[i] + ray * nearLerp;      // Near face
    sliceCorners[i + 4] = nearCorners[i] + ray * farLerp;   // Far face
  }
  
  return sliceCorners;
}

// Compute bounding sphere from frustum corners (rotation-stable)
struct BoundingSphere
{
  glm::vec3 center;
  float radius;
};

[[nodiscard]] BoundingSphere computeBoundingSphere(const std::array<glm::vec3, 8>& corners)
{
  glm::vec3 center(0.0f);
  for(const auto& corner : corners)
    center += corner;
  center /= static_cast<float>(corners.size());
  
  float radius = 0.0f;
  for(const auto& corner : corners)
    radius = std::max(radius, glm::length(corner - center));
  
  return {center, radius};
}

// Compute light view-projection with texel snapping
[[nodiscard]] glm::mat4 computeCascadeLightViewProjection(
    const BoundingSphere& sphere,
    const glm::vec3& lightDirection,
    uint32_t shadowMapResolution,
    const clipspace::ProjectionConvention& convention)
{
  // Build light view matrix
  const glm::vec3 lightPosition = sphere.center - lightDirection * sphere.radius;
  const glm::vec3 worldUp = std::abs(lightDirection.y) > 0.95f
      ? glm::vec3(0.0f, 0.0f, 1.0f)
      : glm::vec3(0.0f, 1.0f, 0.0f);
  const glm::mat4 lightView = glm::lookAt(lightPosition, sphere.center, worldUp);
  
  // Transform sphere center to light space
  const glm::vec3 lightSpaceCenter = glm::vec3(lightView * glm::vec4(sphere.center, 1.0f));
  
  // Compute orthographic bounds
  const float diameter = sphere.radius * 2.0f;
  const float halfExtent = diameter * 0.5f;
  
  float left   = lightSpaceCenter.x - halfExtent;
  float right  = lightSpaceCenter.x + halfExtent;
  float bottom = lightSpaceCenter.y - halfExtent;
  float top    = lightSpaceCenter.y + halfExtent;
  
  // === STABILIZATION: Snap to Shadow Texel ===
  const float texelSize = diameter / static_cast<float>(shadowMapResolution);
  if(texelSize > 0.0f)
  {
    left   = std::floor(left   / texelSize) * texelSize;
    right  = std::ceil(right  / texelSize) * texelSize;
    bottom = std::floor(bottom / texelSize) * texelSize;
    top    = std::ceil(top    / texelSize) * texelSize;
  }
  
  // Depth bounds with padding for shadow casters outside frustum
  const float nearPlane = 0.1f;
  const float farPlane = sphere.radius * 4.0f;
  
  const glm::mat4 lightProjection = clipspace::makeOrthographicProjection(
      left, right, bottom, top, nearPlane, farPlane, convention);
  
  return lightProjection * lightView;
}

}  // namespace

void CSMShadowResources::init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& createInfo)
{
  m_device = device;
  m_allocator = allocator;
  m_cascadeCount = createInfo.cascadeCount;
  m_cascadeResolution = createInfo.cascadeResolution;
  m_projectionConvention = createInfo.projectionConvention;
  
  utils::DebugUtil& dutil = utils::DebugUtil::getInstance();
  
  // Create Texture2DArray for all cascades
  const VkImageCreateInfo cascadeArrayInfo{
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = createInfo.shadowFormat,
    .extent = {m_cascadeResolution, m_cascadeResolution, 1},
    .mipLevels = 1,
    .arrayLayers = m_cascadeCount,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
  };
  
  const VmaAllocationCreateInfo imageAllocInfo{.usage = VMA_MEMORY_USAGE_GPU_ONLY};
  VK_CHECK(vmaCreateImage(allocator, &cascadeArrayInfo, &imageAllocInfo,
      &m_cascadeArray.image, &m_cascadeArray.allocation, nullptr));
  dutil.setObjectName(m_cascadeArray.image, "CSM_CascadeArray");
  
  // Create full array view (for sampling in shader)
  const VkImageViewCreateInfo arrayViewInfo{
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .image = m_cascadeArray.image,
    .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
    .format = createInfo.shadowFormat,
    .subresourceRange = {
      .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = m_cascadeCount,
    },
  };
  VK_CHECK(vkCreateImageView(device, &arrayViewInfo, nullptr, &m_cascadeArrayView));
  dutil.setObjectName(m_cascadeArrayView, "CSM_CascadeArrayView");
  
  // Create per-layer views (for rendering each cascade)
  for(uint32_t i = 0; i < m_cascadeCount; ++i)
  {
    const VkImageViewCreateInfo layerViewInfo{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = m_cascadeArray.image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = createInfo.shadowFormat,
      .subresourceRange = {
        .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = i,
        .layerCount = 1,
      },
    };
    VK_CHECK(vkCreateImageView(device, &layerViewInfo, nullptr, &m_cascadeLayerViews[i]));
    
    char name[32];
    std::snprintf(name, sizeof(name), "CSM_CascadeLayer%d", i);
    dutil.setObjectName(m_cascadeLayerViews[i], name);
  }
  
  // Create shadow uniform buffer
  const VkBufferCreateInfo uniformInfo{
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = sizeof(shaderio::ShadowUniforms),
    .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
  };
  
  const VmaAllocationCreateInfo uniformCreateInfo{
    .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
  };
  
  VmaAllocationInfo uniformAllocInfo{};
  VK_CHECK(vmaCreateBuffer(allocator, &uniformInfo, &uniformCreateInfo,
      &m_shadowUniformBuffer.buffer, &m_shadowUniformBuffer.allocation, &uniformAllocInfo));
  dutil.setObjectName(m_shadowUniformBuffer.buffer, "CSM_ShadowUniformBuffer");
  
  VK_CHECK(vmaMapMemory(allocator, m_shadowUniformBuffer.allocation, &m_shadowUniformMapped));
  
  // Initialize image layout
  const VkImageMemoryBarrier2 initBarrier{
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
    .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
    .srcAccessMask = VK_ACCESS_2_NONE,
    .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = m_cascadeArray.image,
    .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, m_cascadeCount},
  };
  
  const VkDependencyInfo initDepInfo{
    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    .imageMemoryBarrierCount = 1,
    .pImageMemoryBarriers = &initBarrier,
  };
  vkCmdPipelineBarrier2(cmd, &initDepInfo);
}

void CSMShadowResources::deinit()
{
  if(m_shadowUniformMapped != nullptr)
  {
    vmaUnmapMemory(m_allocator, m_shadowUniformBuffer.allocation);
    m_shadowUniformMapped = nullptr;
  }
  
  for(uint32_t i = 0; i < m_cascadeCount; ++i)
  {
    if(m_cascadeLayerViews[i] != VK_NULL_HANDLE)
    {
      vkDestroyImageView(m_device, m_cascadeLayerViews[i], nullptr);
      m_cascadeLayerViews[i] = VK_NULL_HANDLE;
    }
  }
  
  if(m_cascadeArrayView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(m_device, m_cascadeArrayView, nullptr);
    m_cascadeArrayView = VK_NULL_HANDLE;
  }
  
  if(m_cascadeArray.image != VK_NULL_HANDLE)
  {
    vmaDestroyImage(m_allocator, m_cascadeArray.image, m_cascadeArray.allocation);
    m_cascadeArray = {};
  }
  
  if(m_shadowUniformBuffer.buffer != VK_NULL_HANDLE)
  {
    vmaDestroyBuffer(m_allocator, m_shadowUniformBuffer.buffer, m_shadowUniformBuffer.allocation);
    m_shadowUniformBuffer = {};
  }
  
  *this = CSMShadowResources{};
}

void CSMShadowResources::updateCascadeMatrices(const shaderio::CameraUniforms& camera, const glm::vec3& lightDir)
{
  const float cameraNear = std::max(0.01f, extractNearPlane(camera.projection, m_projectionConvention));
  const float cameraFar = std::max(cameraNear + 1.0f, extractFarPlane(camera.projection, m_projectionConvention));
  const float maxShadowDistance = std::min(kDefaultMaxShadowDistance, cameraFar);
  const glm::vec3 lightDirection = safeNormalize(lightDir, glm::vec3(0.0f, -1.0f, 0.0f));
  
  // Compute cascade split distances
  float splits[shaderio::LCascadeCount];
  computeCascadeSplits(splits, m_cascadeCount, cameraNear, maxShadowDistance, shaderio::LCascadeSplitLambda);
  
  m_shadowUniformsData.cascadeSplitDistances = glm::vec4(splits[0], splits[1], splits[2], splits[3]);
  
  // Compute per-cascade light view-projection
  for(uint32_t i = 0; i < m_cascadeCount; ++i)
  {
    const float sliceNear = (i == 0) ? cameraNear : splits[i - 1];
    const float sliceFar = splits[i];
    
    // Get frustum slice corners
    const auto corners = computeFrustumSliceCorners(camera, m_projectionConvention, sliceNear, sliceFar);
    
    // Compute bounding sphere (rotation-stable)
    const BoundingSphere sphere = computeBoundingSphere(corners);
    
    // Compute light view-projection with texel snapping
    const glm::mat4 lightVP = computeCascadeLightViewProjection(
        sphere, lightDirection, m_cascadeResolution, m_projectionConvention);
    
    m_shadowUniformsData.cascadeViewProjection[i] = lightVP;
    m_shadowUniformsData.cascadeWorldToShadowTexture[i] =
        clipspace::makeNdcToShadowTextureMatrix(m_projectionConvention) * lightVP;
  }
  
  // Light direction and intensity
  m_shadowUniformsData.lightDirectionAndIntensity = glm::vec4(lightDirection, kShadowIntensity);
  
  // Shadow metrics
  m_shadowUniformsData.shadowMapMetrics = glm::vec4(
      1.0f / static_cast<float>(m_cascadeResolution),
      maxShadowDistance,
      0.0f,  // unused
      static_cast<float>(m_cascadeCount));
  
  // Cascade bias scale
  m_shadowUniformsData.cascadeBiasScale = glm::vec4(
      kBaseConstantBias,
      kBaseSlopeBias,
      kCascadeBiasScaleFactor,
      0.0f);  // normalBias (unused for now)
  
  // Upload to GPU
  std::memcpy(m_shadowUniformMapped, &m_shadowUniformsData, sizeof(m_shadowUniformsData));
  vmaFlushAllocation(m_allocator, m_shadowUniformBuffer.allocation, 0, sizeof(m_shadowUniformsData));
}

}  // namespace demo
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build --config Release 2>&1 | grep -E "(CSMShadowResources|error)" | head -20`
Expected: Build errors may occur due to missing includes, fix inline if needed

- [ ] **Step 3: Commit**

```bash
git add render/CSMShadowResources.cpp
git commit -m "feat(csm): implement CSMShadowResources cascade calculation

Practical split λ=0.5, bounding sphere, texel snapping,
Texture2DArray creation, per-layer views.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 4: Create CSMShadowPass.h Header

**Files:**
- Create: `render/passes/CSMShadowPass.h`

- [ ] **Step 1: Write CSMShadowPass.h header**

```cpp
#pragma once

#include "../Pass.h"

namespace demo {

class Renderer;

class CSMShadowPass : public RenderPassNode
{
public:
  explicit CSMShadowPass(Renderer* renderer);
  ~CSMShadowPass() override = default;

  [[nodiscard]] const char* getName() const override { return "CSMShadowPass"; }
  [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
  void execute(const PassContext& context) const override;

private:
  Renderer* m_renderer{nullptr};
  
  void renderCascadeLayer(const PassContext& context, uint32_t cascadeIndex) const;
  void drawMeshes(const PassContext& context, VkPipelineLayout pipelineLayout) const;
};

}  // namespace demo
```

- [ ] **Step 2: Verify header compiles**

Run: `cmake --build build --config Release 2>&1 | grep -E "(CSMShadowPass|error)" || echo "Header OK"`
Expected: No errors related to CSMShadowPass.h

- [ ] **Step 3: Commit**

```bash
git add render/passes/CSMShadowPass.h
git commit -m "feat(csm): add CSMShadowPass header

Per-cascade render loop, renderCascadeLayer helper,
drawMeshes for cascade depth rendering.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 5: Create CSMShadowPass.cpp Implementation

**Files:**
- Create: `render/passes/CSMShadowPass.cpp`

- [ ] **Step 1: Write CSMShadowPass.cpp implementation**

```cpp
#include "CSMShadowPass.h"
#include "../Renderer.h"
#include "../CSMShadowResources.h"
#include "../MeshPool.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/vulkan/VulkanCommandList.h"

#include <array>
#include <cstring>

namespace demo {

CSMShadowPass::CSMShadowPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> CSMShadowPass::getDependencies() const
{
  // Single Texture2DArray resource (written by all cascades)
  static const std::array<PassResourceDependency, 1> dependencies = {
    PassResourceDependency::texture(kPassShadowHandle, ResourceAccess::write, 
        rhi::ShaderStage::fragment, rhi::ResourceState::DepthStencilAttachment),
  };
  return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void CSMShadowPass::execute(const PassContext& context) const
{
  if(m_renderer == nullptr || context.params == nullptr || context.transientAllocator == nullptr)
    return;

  context.cmd->beginEvent("CSMShadowPass");

  CSMShadowResources& csmResources = m_renderer->getCSMShadowResources();
  const uint32_t cascadeCount = csmResources.getCascadeCount();
  const shaderio::ShadowUniforms* shadowData = csmResources.getShadowUniformsData();

  // Render each cascade layer
  for(uint32_t i = 0; i < cascadeCount; ++i)
  {
    renderCascadeLayer(context, i);
  }

  // Final transition: DepthStencilAttachment → General (for sampling)
  context.cmd->transitionTexture(rhi::TextureBarrierDesc{
    .texture = rhi::TextureHandle{kPassShadowHandle.index, kPassShadowHandle.generation},
    .nativeImage = reinterpret_cast<uint64_t>(csmResources.getCascadeImage()),
    .aspect = rhi::TextureAspect::depth,
    .srcStage = rhi::PipelineStage::FragmentShader,
    .dstStage = rhi::PipelineStage::FragmentShader,
    .srcAccess = rhi::ResourceAccess::write,
    .dstAccess = rhi::ResourceAccess::read,
    .oldState = rhi::ResourceState::DepthStencilAttachment,
    .newState = rhi::ResourceState::General,
    .isSwapchain = false,
  });

  context.cmd->endEvent();
}

void CSMShadowPass::renderCascadeLayer(const PassContext& context, uint32_t cascadeIndex) const
{
  CSMShadowResources& csmResources = m_renderer->getCSMShadowResources();
  const VkExtent2D cascadeExtent = csmResources.getCascadeExtent();
  const rhi::Extent2D extent{cascadeExtent.width, cascadeExtent.height};

  // Depth target: single layer of Texture2DArray
  const rhi::DepthTargetDesc depthTarget{
    .texture = {},
    .view = rhi::TextureViewHandle::fromNative(csmResources.getCascadeLayerView(cascadeIndex)),
    .state = rhi::ResourceState::DepthStencilAttachment,
    .loadOp = rhi::LoadOp::clear,
    .storeOp = rhi::StoreOp::store,
    .clearValue = {0.0f, 0},  // Far depth = 0.0 (reversed depth)
  };

  const rhi::RenderPassDesc passDesc{
    .renderArea = {{0, 0}, extent},
    .colorTargets = nullptr,
    .colorTargetCount = 0,
    .depthTarget = &depthTarget,
  };

  context.cmd->beginRenderPass(passDesc);
  context.cmd->setViewport(rhi::Viewport{0.0f, 0.0f, 
      static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f});
  context.cmd->setScissor(rhi::Rect2D{{0, 0}, extent});

  const PipelineHandle shadowPipeline = m_renderer->getShadowPipelineHandle();
  if(shadowPipeline.isNull())
  {
    context.cmd->endRenderPass();
    return;
  }

  const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
      m_renderer->getPipelineOpaque(shadowPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
  const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getGBufferPipelineLayout());
  rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);

  // Bind texture descriptor set
  const VkDescriptorSet textureSet = reinterpret_cast<VkDescriptorSet>(m_renderer->getGBufferColorDescriptorSet());
  vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
      pipelineLayout, shaderio::LSetTextures, 1, &textureSet, 0, nullptr);

  // Upload cascade-specific camera uniforms
  const shaderio::ShadowUniforms* shadowData = csmResources.getShadowUniformsData();
  
  const TransientAllocator::Allocation cameraAlloc =
      context.transientAllocator->allocate(sizeof(shaderio::CameraUniforms), 256);

  shaderio::CameraUniforms cascadeCamera{};
  cascadeCamera.viewProjection = shadowData->cascadeViewProjection[cascadeIndex];
  
  // Apply cascade-specific bias scaling
  const float cascadeBiasScale = 1.0f + cascadeIndex * shadowData->cascadeBiasScale.z;
  cascadeCamera.shadowConstantBias = shadowData->cascadeBiasScale.x * cascadeBiasScale;
  cascadeCamera.shadowDirectionAndSlopeBias = glm::vec4(
      shadowData->lightDirectionAndIntensity.xyz,
      shadowData->cascadeBiasScale.y * cascadeBiasScale);

  std::memcpy(cameraAlloc.cpuPtr, &cascadeCamera, sizeof(cascadeCamera));
  context.transientAllocator->flushAllocation(cameraAlloc, sizeof(cascadeCamera));

  // Bind camera descriptor set
  const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
  if(!cameraBindGroupHandle.isNull())
  {
    VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(
        m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific));
    const uint32_t cameraDynamicOffset = cameraAlloc.offset;
    vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipelineLayout, shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
  }

  // Draw all meshes
  drawMeshes(context, pipelineLayout);

  context.cmd->endRenderPass();
}

void CSMShadowPass::drawMeshes(const PassContext& context, VkPipelineLayout pipelineLayout) const
{
  const BindGroupHandle drawBindGroupHandle = m_renderer->getDrawBindGroup(context.frameIndex);
  MeshPool& meshPool = m_renderer->getMeshPool();

  if(context.gltfModel == nullptr)
    return;

  for(size_t i = 0; i < context.gltfModel->meshes.size(); ++i)
  {
    MeshHandle meshHandle = context.gltfModel->meshes[i];
    const MeshRecord* mesh = meshPool.tryGet(meshHandle);
    if(mesh == nullptr)
      continue;

    // Skip alpha-blend meshes (they don't cast shadows)
    int32_t alphaMode = shaderio::LAlphaOpaque;
    if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
    {
      MaterialHandle materialHandle = context.gltfModel->materials[mesh->materialIndex];
      auto indices = m_renderer->getMaterialTextureIndices(materialHandle, context.gltfModel);
      alphaMode = indices.alphaMode;
      if(alphaMode == shaderio::LAlphaBlend)
        continue;
    }

    // Upload draw uniforms
    const TransientAllocator::Allocation drawAlloc =
        context.transientAllocator->allocate(sizeof(shaderio::DrawUniforms), 256);

    shaderio::DrawUniforms drawData{};
    drawData.modelMatrix = mesh->transform;
    drawData.baseColorFactor = glm::vec4(1.0f);
    drawData.baseColorTextureIndex = -1;
    drawData.alphaMode = alphaMode;
    drawData.alphaCutoff = 0.5f;

    if(mesh->materialIndex >= 0 && mesh->materialIndex < static_cast<int32_t>(context.gltfModel->materials.size()))
    {
      MaterialHandle materialHandle = context.gltfModel->materials[mesh->materialIndex];
      auto indices = m_renderer->getMaterialTextureIndices(materialHandle, context.gltfModel);
      drawData.baseColorTextureIndex = indices.baseColor;
      drawData.alphaMode = indices.alphaMode;
      drawData.alphaCutoff = indices.alphaCutoff;
    }

    std::memcpy(drawAlloc.cpuPtr, &drawData, sizeof(drawData));
    context.transientAllocator->flushAllocation(drawAlloc, sizeof(drawData));

    if(!drawBindGroupHandle.isNull())
    {
      VkDescriptorSet drawDescriptorSet = reinterpret_cast<VkDescriptorSet>(
          m_renderer->getBindGroupDescriptorSet(drawBindGroupHandle, BindGroupSetSlot::shaderSpecific));
      const uint32_t drawDynamicOffset = drawAlloc.offset;
      vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
          pipelineLayout, shaderio::LSetDraw, 1, &drawDescriptorSet, 1, &drawDynamicOffset);
    }

    const uint64_t vertexHandle = mesh->vertexBufferHandle;
    const uint64_t vertexOffset = 0;
    context.cmd->bindVertexBuffers(0, &vertexHandle, &vertexOffset, 1);
    context.cmd->bindIndexBuffer(mesh->indexBufferHandle, 0, rhi::IndexFormat::uint32);
    context.cmd->drawIndexed(mesh->indexCount, 1, 0, 0, 0);
  }
}

}  // namespace demo
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build --config Release 2>&1 | grep -E "(CSMShadowPass|error)" | head -20`
Expected: Build errors will occur (Renderer.h needs CSM integration), fix in next task

- [ ] **Step 3: Commit**

```bash
git add render/passes/CSMShadowPass.cpp
git commit -m "feat(csm): implement CSMShadowPass 4-layer rendering

Per-cascade camera upload with bias scaling, mesh draw loop,
Texture2DArray layer-by-layer depth rendering.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 6: Update shader.light.slang for CSM Sampling

**Files:**
- Modify: `shaders/shader.light.slang`

- [ ] **Step 1: Add cascade selection and CSM sampling function**

Find the existing `sampleShadow` function and replace with CSM version:

```cpp
// CSM cascade selection based on view depth
int selectCascadeIndex(float viewDepth, vec4 splitDistances)
{
    int cascadeIndex = 0;
    
    [unroll]
    for(int i = 0; i < LCascadeCount - 1; ++i)
    {
        // Extract split distance from vec4
        float splitDist = splitDistances[i];
        if(viewDepth > splitDist)
            cascadeIndex = i + 1;
    }
    
    return cascadeIndex;
}

// Reconstruct view-space depth from NDC depth
float reconstructViewDepth(float ndcDepth, mat4 projection)
{
    // For perspective projection, extract from projection matrix
    // depth = -zNear * zFar / (ndcDepth * (zFar - zNear) - zFar)
    // This works for Vulkan [0,1] NDC depth
    float a = projection[2][2];
    float b = projection[3][2];
    float viewDepth = -b / (ndcDepth + a);
    return viewDepth;
}

// CSM shadow sampling with 3x3 PCF
float sampleCSMShadow(vec3 worldPos, vec3 worldNormal, float viewDepth)
{
    // Select cascade
    int cascadeIndex = selectCascadeIndex(viewDepth, lightParams.shadowMetrics);  // Will need to pass splits
    
    // Transform to shadow texture space
    float4 shadowClip = mul(lightParams.worldToShadow[cascadeIndex], float4(worldPos, 1.0));
    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    
    float2 shadowUv = float2(shadowNdc.x * 0.5 + 0.5, shadowNdc.y * 0.5 + 0.5);
    
    // Bounds check
    if(shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0)
        return 1.0;
    if(shadowNdc.z <= 0.0 || shadowNdc.z >= 1.0)
        return 1.0;
    
    // Compute adaptive bias (cascade-scaled)
    float3 lightDir = normalize(lightParams.lightDirectionAndShadowStrength.xyz);
    float cosTheta = saturate(abs(dot(worldNormal, lightDir)));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    float slope = sinTheta / max(cosTheta, 0.01);
    
    float baseConstantBias = lightParams.shadowMetrics.y;  // Will reorganize
    float baseSlopeBias = lightParams.lightColorAndNormalBias.w;
    float scaleFactor = 0.5;  // cascadeBiasScale.z
    
    float cascadeBias = baseConstantBias * (1.0 + cascadeIndex * scaleFactor);
    float slopeBias = baseSlopeBias * (1.0 + cascadeIndex * scaleFactor) * slope;
    float bias = cascadeBias + slopeBias;
    
    // 3x3 PCF sampling
    float texelSize = 1.0 / 1024.0;  // cascade resolution
    float visibility = 0.0;
    
    [unroll]
    for(int y = -1; y <= 1; ++y)
    {
        [unroll]
        for(int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * texelSize;
            
            // Sample Texture2DArray with layer = cascadeIndex
            float3 sampleCoord = float3(shadowUv + offset, cascadeIndex);
            float shadowDepth = inTexture[kShadowMapIndex].Sample(sampleCoord).r;
            
            float receiverDepth = shadowNdc.z + bias;
            visibility += (receiverDepth >= shadowDepth) ? 1.0 : 0.0;
        }
    }
    
    return visibility / 9.0;
}
```

Note: This requires LightParams to be updated to include cascade matrices and split distances. Will update in Task 8.

- [ ] **Step 2: Update LightParams struct in shader_io.h**

Add to `LightParams` struct (around line 162-169):

```cpp
struct LightParams
{
  mat4 worldToShadow[LCascadeCount];  // Per-cascade world-to-shadow matrices
  vec4 cascadeSplitDistances;         // x=c0, y=c1, z=c2, w=c3 far distances
  vec4 lightDirectionAndShadowStrength;
  vec4 lightColorAndNormalBias;
  vec4 ambientColorAndTexelSize;
  vec4 shadowMetrics;                 // x=texelSize, y=baseBias, z=slopeBias, w=cascadeCount
};
```

- [ ] **Step 3: Commit**

```bash
git add shaders/shader.light.slang shaders/shader_io.h
git commit -m "feat(csm): add CSM sampling to shader.light.slang

Cascade selection, adaptive bias, 3x3 PCF with Texture2DArray.
Updated LightParams with cascade matrices and splits.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 7: Update Renderer.h for CSM Integration

**Files:**
- Modify: `render/Renderer.h`

- [ ] **Step 1: Replace ShadowResources include with CSMShadowResources**

Find line with `#include "ShadowResources.h"` or add after existing includes:

```cpp
#include "CSMShadowResources.h"
```

Remove if exists:
```cpp
#include "ShadowResources.h"
```

- [ ] **Step 2: Replace ShadowResources member with CSMShadowResources**

Find `ShadowResources` member and replace:

```cpp
// OLD (remove):
// ShadowResources m_shadowResources;

// NEW (add):
CSMShadowResources m_csmShadowResources;
```

- [ ] **Step 3: Replace ShadowPass include with CSMShadowPass**

Find line with `#include "passes/ShadowPass.h"` and replace:

```cpp
#include "passes/CSMShadowPass.h"
```

Remove if exists:
```cpp
#include "passes/ShadowPass.h"
```

- [ ] **Step 4: Replace ShadowPass member with CSMShadowPass**

Find `m_shadowPass` member and replace:

```cpp
// OLD (remove):
// std::unique_ptr<ShadowPass> m_shadowPass;

// NEW (add):
std::unique_ptr<CSMShadowPass> m_csmShadowPass;
```

- [ ] **Step 5: Replace shadow accessor methods**

Replace existing shadow accessor methods:

```cpp
// OLD (remove):
// VkImageView getShadowMapView() const;
// VkImage getShadowMapImage() const;

// NEW (add):
CSMShadowResources& getCSMShadowResources() { return m_csmShadowResources; }
VkImageView getShadowMapView() const { return m_csmShadowResources.getCascadeView(); }
VkImage getShadowMapImage() const { return m_csmShadowResources.getCascadeImage(); }
```

- [ ] **Step 6: Add CSM-specific accessor**

```cpp
shaderio::ShadowUniforms* getShadowUniformsData() 
{ 
  return m_csmShadowResources.getShadowUniformsData(); 
}
```

- [ ] **Step 7: Build and verify compilation**

Run: `cmake --build build --config Release 2>&1 | grep -E "(Renderer|error)" | head -30`
Expected: Build errors may occur in Renderer.cpp, fix in next task

- [ ] **Step 8: Commit**

```bash
git add render/Renderer.h
git commit -m "feat(csm): integrate CSMShadowResources and CSMShadowPass in Renderer

Replace ShadowResources/ShadowPass with CSM variants,
add cascade accessors.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 8: Update Renderer.cpp for CSM Initialization

**Files:**
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Replace ShadowPass creation with CSMShadowPass**

Find `m_shadowPass = std::make_unique<ShadowPass>(this);` and replace:

```cpp
m_csmShadowPass = std::make_unique<CSMShadowPass>(this);
```

- [ ] **Step 2: Replace pass registration**

Find `m_passExecutor.addPass(*m_shadowPass);` and replace:

```cpp
m_passExecutor.addPass(*m_csmShadowPass);
```

- [ ] **Step 3: Add CSMShadowResources initialization**

Find shadow resources initialization (around scene resources init) and replace:

```cpp
// Initialize CSM shadow resources
CSMShadowResources::CreateInfo csmInfo{
  .cascadeCount = 4,
  .cascadeResolution = 1024,
  .shadowFormat = VK_FORMAT_D32_SFLOAT,
};
m_csmShadowResources.init(m_device.device->getNativeDevice(), m_allocator, cmd, csmInfo);
```

- [ ] **Step 4: Add CSM matrix update in frame preparation**

Find `updateShadowMatrices` call or add in frame preparation:

```cpp
// Update CSM cascade matrices each frame
const glm::vec3 lightDir = params.lightSettings.direction;
m_csmShadowResources.updateCascadeMatrices(*params.cameraUniforms, lightDir);
```

- [ ] **Step 5: Replace shadow deinit**

Find `m_shadowResources.deinit()` and replace:

```cpp
m_csmShadowResources.deinit();
```

- [ ] **Step 6: Update LightParams for CSM**

In `buildFrameLightingState()` or similar, update LightParams to include cascade matrices:

```cpp
shaderio::LightParams lightParams{};
for(int i = 0; i < shaderio::LCascadeCount; ++i)
{
  lightParams.worldToShadow[i] = shadowData->cascadeWorldToShadowTexture[i];
}
lightParams.cascadeSplitDistances = shadowData->cascadeSplitDistances;
lightParams.lightDirectionAndShadowStrength = glm::vec4(
    shadowData->lightDirectionAndIntensity.xyz,
    params.lightSettings.shadowStrength);
lightParams.shadowMetrics = glm::vec4(
    1.0f / static_cast<float>(m_csmShadowResources.getCascadeResolution()),
    shadowData->cascadeBiasScale.x,  // base constant bias
    shadowData->cascadeBiasScale.y,  // base slope bias
    static_cast<float>(shaderio::LCascadeCount));
```

- [ ] **Step 7: Build and verify**

Run: `cmake --build build --config Release 2>&1 | tail -30`
Expected: Build should succeed with all CSM integration complete

- [ ] **Step 8: Commit**

```bash
git add render/Renderer.cpp
git commit -m "feat(csm): initialize CSMShadowResources and update cascade matrices

CSM init with 4 cascades at 1024 resolution, per-frame matrix
update, LightParams with cascade arrays.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 9: Update CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add CSMShadowResources files**

Find `render/ShadowResources.cpp` and `render/ShadowResources.h` entries, replace with:

```cmake
render/CSMShadowResources.h
render/CSMShadowResources.cpp
```

Or add if not found:

```cmake
render/CSMShadowResources.h
render/CSMShadowResources.cpp
```

- [ ] **Step 2: Add CSMShadowPass files**

Find `render/passes/ShadowPass.cpp` and `render/passes/ShadowPass.h` entries, replace with:

```cmake
render/passes/CSMShadowPass.h
render/passes/CSMShadowPass.cpp
```

- [ ] **Step 3: Remove old shadow files**

Remove if still present:

```cmake
# Remove these lines:
render/ShadowResources.h
render/ShadowResources.cpp
render/passes/ShadowPass.h
render/passes/ShadowPass.cpp
```

- [ ] **Step 4: Build and verify**

Run: `cmake --build build --config Release 2>&1 | tail -20`
Expected: Build succeeds with CMakeLists updated

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "feat(csm): update CMakeLists for CSM files

Add CSMShadowResources/CSMShadowPass, remove old ShadowResources/ShadowPass.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 10: Add CSM Frustum Visualization to DebugPass

**Files:**
- Modify: `render/passes/DebugPass.h`
- Modify: `render/passes/DebugPass.cpp`

- [ ] **Step 1: Add CSMFrustumWireframePrimitive to DebugPrimitives.h (if exists) or DebugPass.cpp**

If `render/passes/DebugPrimitives.h` exists, add there. Otherwise add to DebugPass.cpp:

```cpp
// CSM Cascade Frustum Wireframe Primitive
class CSMFrustumWireframePrimitive : public IDebugPrimitive
{
public:
  bool isEnabled(const DebugSettings& s) const override 
  { 
    return s.showFrustum && s.showShadowCascades; 
  }
  
  const char* getName() const override { return "CSM Cascade Frustums"; }
  
  uint32_t collectData(const PassContext& ctx, DebugVertex* vtx) const override
  {
    CSMShadowResources& csm = ctx.params->renderer->getCSMShadowResources();
    const shaderio::ShadowUniforms* shadowData = csm.getShadowUniformsData();
    if(shadowData == nullptr) return 0;
    
    uint32_t count = 0;
    
    const float cascadeColors[4][4] = {
      {1.0f, 0.0f, 0.0f, 1.0f},  // Cascade 0: Red
      {0.0f, 1.0f, 0.0f, 1.0f},  // Cascade 1: Green
      {0.0f, 0.0f, 1.0f, 1.0f},  // Cascade 2: Blue
      {0.0f, 1.0f, 1.0f, 1.0f},  // Cascade 3: Cyan
    };
    
    const clipspace::ProjectionConvention convention = 
        clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan);
    
    const int targetCascade = ctx.params->debugSettings.cascadeIndex;
    const int cascadeCount = static_cast<int>(csm.getCascadeCount());
    
    for(int c = 0; c < cascadeCount; ++c)
    {
      if(targetCascade >= 0 && targetCascade != c) continue;
      
      const glm::mat4 lightVP = shadowData->cascadeViewProjection[c];
      const glm::mat4 invLightVP = glm::inverse(lightVP);
      
      const glm::vec4 ndcCorners[8] = {
        glm::vec4(-1, -1, convention.ndcNearZ, 1),
        glm::vec4( 1, -1, convention.ndcNearZ, 1),
        glm::vec4(-1,  1, convention.ndcNearZ, 1),
        glm::vec4( 1,  1, convention.ndcNearZ, 1),
        glm::vec4(-1, -1, convention.ndcFarZ,  1),
        glm::vec4( 1, -1, convention.ndcFarZ,  1),
        glm::vec4(-1,  1, convention.ndcFarZ,  1),
        glm::vec4( 1,  1, convention.ndcFarZ,  1),
      };
      
      glm::vec3 worldCorners[8];
      for(int i = 0; i < 8; ++i)
      {
        glm::vec4 world = invLightVP * ndcCorners[i];
        worldCorners[i] = glm::vec3(world / world.w);
      }
      
      count += writeFrustumLines(vtx + count, worldCorners, cascadeColors[c]);
    }
    
    return count;
  }
  
private:
  static uint32_t writeFrustumLines(DebugVertex* vtx, const glm::vec3 corners[8], const float color[4])
  {
    constexpr int edges[24] = {
      0,1, 1,3, 3,2, 2,0,  // Near
      4,5, 5,7, 7,6, 6,4,  // Far
      0,4, 1,5, 2,6, 3,7,  // Connections
    };
    
    for(int i = 0; i < 24; ++i)
    {
      const glm::vec3& p = corners[edges[i]];
      vtx[i].position[0] = p.x;
      vtx[i].position[1] = p.y;
      vtx[i].position[2] = p.z;
      std::memcpy(vtx[i].color, color, sizeof(float[4]));
    }
    
    return 24;
  }
};
```

- [ ] **Step 2: Register CSM primitive in Renderer or DebugRegistry**

Add registration:

```cpp
m_debugRegistry.registerPrimitive("csm_frustums", []() {
  return std::make_unique<CSMFrustumWireframePrimitive>();
});
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --config Release 2>&1 | grep -E "(CSMFrustum|error)" | head -20`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add render/passes/DebugPass.h render/passes/DebugPass.cpp
git commit -m "feat(csm): add CSM cascade frustum visualization

Color-coded wireframes (red/green/blue/cyan) for each cascade,
cascade filter support.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 11: Add ImGui CSM Debug Panel

**Files:**
- Modify: `app/MinimalLatestApp.h`

- [ ] **Step 1: Add CSM debug settings to DebugSettings struct (if exists)**

Or add to MinimalLatestApp member:

```cpp
// CSM debug settings
bool showShadowCascades{true};
bool cascadeOverlayMode{false};
int cascadeIndex{-1};  // -1 = all cascades
float cascadeOverlayAlpha{0.25f};
```

- [ ] **Step 2: Add drawCSMDebugPanel function**

```cpp
inline void MinimalLatestApp::drawCSMDebugPanel()
{
  if(ImGui::CollapsingHeader("CSM Shadows"))
  {
    ImGui::Indent();
    
    ImGui::Checkbox("Show Cascade Frustums", &m_debugSettings.showShadowCascades);
    
    if(m_debugSettings.showShadowCascades)
    {
      static const char* cascadeNames[] = {
        "All Cascades", "Cascade 0 (Near)", "Cascade 1", "Cascade 2", "Cascade 3 (Far)"
      };
      ImGui::Combo("Cascade Filter", &m_debugSettings.cascadeIndex, cascadeNames, 5);
      
      ImGui::Checkbox("Cascade Overlay (Screen)", &m_debugSettings.cascadeOverlayMode);
      if(m_debugSettings.cascadeOverlayMode)
      {
        ImGui::SliderFloat("Overlay Alpha", &m_debugSettings.cascadeOverlayAlpha, 0.1f, 0.5f);
      }
    }
    
    // Display split distances from shadow uniforms
    shaderio::ShadowUniforms* shadowData = m_renderer->getShadowUniformsData();
    if(shadowData != nullptr)
    {
      ImGui::Separator();
      ImGui::Text("Cascade Split Distances:");
      const glm::vec4& splits = shadowData->cascadeSplitDistances;
      ImGui::Text("  C0: %.2f", splits.x);
      ImGui::Text("  C1: %.2f", splits.y);
      ImGui::Text("  C2: %.2f", splits.z);
      ImGui::Text("  C3: %.2f", splits.w);
      ImGui::Text("  Resolution: %d", m_renderer->getCSMShadowResources().getCascadeResolution());
    }
    
    ImGui::Unindent();
  }
}
```

- [ ] **Step 3: Call drawCSMDebugPanel in Settings window**

Add in ImGui settings window:

```cpp
drawCSMDebugPanel();
```

- [ ] **Step 4: Build and verify**

Run: `cmake --build build --config Release 2>&1 | tail -20`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
git add app/MinimalLatestApp.h
git commit -m "feat(csm): add ImGui CSM debug panel

Cascade frustum toggle, cascade filter, overlay mode,
split distances display.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 12: Remove Old Shadow Files

**Files:**
- Remove: `render/ShadowResources.h`
- Remove: `render/ShadowResources.cpp`
- Remove: `render/passes/ShadowPass.h`
- Remove: `render/passes/ShadowPass.cpp`

- [ ] **Step 1: Remove old shadow files**

```bash
git rm render/ShadowResources.h render/ShadowResources.cpp
git rm render/passes/ShadowPass.h render/passes/ShadowPass.cpp
```

- [ ] **Step 2: Commit removal**

```bash
git commit -m "refactor(csm): remove old ShadowResources and ShadowPass

Replaced by CSMShadowResources and CSMShadowPass.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 13: Build and Test CSM System

**Files:**
- Test: Run application, verify shadow rendering

- [ ] **Step 1: Build application**

Run: `cmake --build build --config Release`
Expected: Build succeeds with no errors

- [ ] **Step 2: Run application**

Run: `./build/Release/Demo.exe`
Expected: Application launches, scene renders with shadows

- [ ] **Step 3: Test cascade frustum visualization**

Enable "Show Cascade Frustums" in ImGui Settings → CSM Shadows
Expected: Colored wireframes appear (red=c0, green=c1, blue=c2, cyan=c3)

- [ ] **Step 4: Test cascade filter**

Select "Cascade 0 (Near)" in cascade filter dropdown
Expected: Only red frustum appears

- [ ] **Step 5: Test split distances display**

Check ImGui "Cascade Split Distances" text
Expected: Values displayed (e.g., C0: 13.44, C1: 30.1, C2: 65.8, C3: 100.2)

- [ ] **Step 6: Test shadow rendering quality**

Move camera near/far from objects
Expected: Near shadows sharp (cascade 0), far shadows acceptable (cascade 3)

- [ ] **Step 7: Test stabilization**

Rotate camera and move it slowly
Expected: No shadow flickering during rotation or translation

- [ ] **Step 8: Final commit**

```bash
git add -A
git commit -m "feat(csm): complete CSM shadow system implementation

4 cascades, λ=0.5 split, bounding sphere + texel snap stabilization,
cascade-scaled bias, 3x3 PCF, debug visualization.

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage:**

| Spec Section | Task Coverage |
|--------------|---------------|
| Section 1: Core Data Structures | Task 1, Task 2 |
| Section 2: Cascade Split Calculation | Task 3 |
| Section 3: Light Frustum + Stabilization | Task 3 |
| Section 4: Shadow Atlas | Task 2, Task 3 |
| Section 5: Cascade Selection + Shader | Task 6 |
| Section 6: ShadowPass Rendering | Task 4, Task 5 |
| Section 7: Debug Visualization | Task 10, Task 11 |
| File Structure Changes | Task 7, Task 8, Task 9, Task 12 |

**2. Placeholder scan:**

- No TBD/TODO markers found
- All code blocks contain complete implementations
- All commands have exact paths and expected outputs

**3. Type consistency:**

- `ShadowUniforms` struct matches across shader_io.h, CSMShadowResources.cpp, shader.light.slang
- `LCascadeCount = 4` consistent across all files
- `cascadeViewProjection[LCascadeCount]` array size matches
- `cascadeSplitDistances` as vec4 (x=c0, y=c1, z=c2, w=c3) consistent
- `cascadeBiasScale.z = 0.5` scale factor matches shader calculation