# CSM (Cascaded Shadow Maps) Design Specification

**Date:** 2026-04-14
**Status:** Approved
**Scope:** Replace existing single shadow map with 4-cascade CSM system

---

## Overview

Upgrade the current directional shadow system from a single 2048x2048 shadow map to a 4-cascade CSM (Cascaded Shadow Maps) system using Texture2DArray storage. This provides higher shadow quality across the full view distance by allocating more shadow resolution to near-field geometry.

---

## Design Decisions Summary

| Feature | Choice | Rationale |
|---------|--------|-----------|
| Cascade Count | 4 | Industry standard, good quality/performance balance |
| Per-Cascade Resolution | 1024x1024 | Standard quality, ~16MB total memory |
| Split Strategy | λ=0.5 Practical Split | Balanced log+linear, good for both near and far |
| Stabilization | Bounding Sphere + Texel Snap | Eliminates rotation + translation flickering |
| Cascade Blending | Hard Boundaries | Simpler implementation, no blending overhead |
| PCF Filtering | 3x3 Uniform | Consistent quality, matches existing shadow |
| Adaptive Bias | Cascade-scaled (1 + i*0.5) | Simple scaling, reduces acne/peter-panning |
| Debug Visualization | Frustum Wireframes + Overlay | Complete debugging toolkit |

---

## Architecture

### Pipeline Position

```
CSMShadowPass → GBufferPass → LightCullingPass → LightPass → DebugPass → ImguiPass → PresentPass
```

CSMShadowPass replaces ShadowPass, rendering 4 cascade layers before GBuffer.

### Component Structure

```
render/
├── CSMShadowResources.h       (new: Texture2DArray + cascade matrices)
├── CSMShadowResources.cpp     (new: cascade calculation + stabilization)
├── passes/
│   ├── CSMShadowPass.h        (new: per-cascade render loop)
│   ├── CSMShadowPass.cpp      (new: 4-layer depth rendering)
│   └── DebugPass.cpp          (modified: add CSM debug primitives)
├── Renderer.h                 (modified: replace ShadowResources with CSM)
└── Renderer.cpp               (modified: CSM initialization)

shaders/
├── shader_io.h                (modified: ShadowUniforms + CSM constants)
├── shader.shadow.slang        (modified: cascade-aware depth shader)
├── shader.light.slang         (modified: CSM sampling function)
└── shader.debug.slang         (modified: cascade overlay shader)
```

---

## Section 1: Core Data Structures

### 1.1 Cascade Configuration Constants

```cpp
// In shader_io.h
STATIC_CONST int LCascadeCount = 4;  // Number of shadow cascades
STATIC_CONST float LCascadeSplitLambda = 0.5f;  // Practical split (log + linear mix)
STATIC_CONST float LCascadeBlendRegion = 0.0f;  // Hard boundaries (no blending)
```

### 1.2 ShadowUniforms Structure (Extended for CSM)

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

### 1.3 CSMShadowResources Class

```cpp
class CSMShadowResources
{
public:
  struct CreateInfo
  {
    uint32_t cascadeCount{4};
    uint32_t cascadeResolution{1024};  // Per cascade
    VkFormat shadowFormat{VK_FORMAT_D32_SFLOAT};
  };

  void init(VkDevice device, VmaAllocator allocator, VkCommandBuffer cmd, const CreateInfo& info);
  void deinit();

  void updateCascadeMatrices(const shaderio::CameraUniforms& camera, const glm::vec3& lightDir);

  // Per-cascade access
  VkImage getCascadeImage() const { return m_cascadeArray.image; }  // Texture2DArray
  VkImageView getCascadeView() const { return m_cascadeArrayView; }
  VkImageView getCascadeLayerView(uint32_t index) const { return m_cascadeLayerViews[index]; }
  uint32_t getCascadeCount() const { return m_cascadeCount; }
  uint32_t getCascadeResolution() const { return m_cascadeResolution; }

  shaderio::ShadowUniforms* getShadowUniformsData() { return &m_shadowUniformsData; }
  VkBuffer getShadowUniformBuffer() const { return m_shadowUniformBuffer.buffer; }

private:
  utils::Image m_cascadeArray;  // VK_IMAGE_TYPE_2D, arrayLayers = cascadeCount
  VkImageView m_cascadeArrayView{VK_NULL_HANDLE};
  VkImageView m_cascadeLayerViews[LCascadeCount];  // Per-layer views for rendering

  utils::Buffer m_shadowUniformBuffer{};
  shaderio::ShadowUniforms m_shadowUniformsData{};
  void* m_shadowUniformMapped{nullptr};

  uint32_t m_cascadeCount{4};
  uint32_t m_cascadeResolution{1024};
};
```

---

## Section 2: Cascade Split Calculation

### 2.1 Split Distance Algorithm (Practical Split λ=0.5)

```cpp
void computeCascadeSplits(float* splits, uint32_t count, float near, float far, float lambda)
{
  for(uint32_t i = 0; i < count; ++i)
  {
    // Uniform distribution fraction
    const float uniformFraction = static_cast<float>(i + 1) / static_cast<float>(count);
    const float uniformSplit = near + (far - near) * uniformFraction;

    // Logarithmic distribution fraction (better near-field resolution)
    const float logFraction = static_cast<float>(i + 1) / static_cast<float>(count);
    const float logSplit = near * std::pow(far / near, logFraction);

    // Practical split: blend log and uniform
    splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
  }
}
```

### 2.2 Split Distance Example

For camera near=0.1, far=100, λ=0.5, 4 cascades:

| Cascade | Uniform Split | Log Split | Practical Split (λ=0.5) |
|---------|---------------|-----------|-------------------------|
| 0 | 25.1 | 1.78 | 13.44 |
| 1 | 50.2 | 10.0 | 30.1 |
| 2 | 75.3 | 56.2 | 65.8 |
| 3 | 100.4 | 100.0 | 100.2 |

Result: Cascade 0 covers [0.1, 13.44], Cascade 1 covers [13.44, 30.1], etc.

---

## Section 3: Light Frustum Calculation & Stabilization

### 3.1 Bounding Sphere Calculation (Rotation-Stable)

```cpp
struct BoundingSphere
{
  glm::vec3 center;
  float radius;
};

BoundingSphere computeBoundingSphere(const std::array<glm::vec3, 8>& corners)
{
  // Compute frustum center (average of corners)
  glm::vec3 center(0.0f);
  for(const auto& corner : corners)
    center += corner;
  center /= static_cast<float>(corners.size());

  // Compute radius (max distance from center to any corner)
  float radius = 0.0f;
  for(const auto& corner : corners)
    radius = std::max(radius, glm::length(corner - center));

  return {center, radius};
}
```

**Why Bounding Sphere vs AABB:**
- AABB bounds change when camera rotates → shadow map projection changes → flickering
- Sphere bounds are rotation-independent → stable shadows during camera rotation

### 3.2 Light View-Projection Construction with Texel Snapping

```cpp
glm::mat4 computeCascadeLightViewProjection(
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
  left   = std::floor(left  / texelSize) * texelSize;
  right  = std::ceil(right / texelSize) * texelSize;
  bottom = std::floor(bottom / texelSize) * texelSize;
  top    = std::ceil(top   / texelSize) * texelSize;

  // Depth bounds with padding
  const float nearPlane = 0.1f;
  const float farPlane = sphere.radius * 4.0f;

  const glm::mat4 lightProjection = clipspace::makeOrthographicProjection(
      left, right, bottom, top, nearPlane, farPlane, convention);

  return lightProjection * lightView;
}
```

### 3.3 Texel Snapping Explanation

Without snapping: Camera moves 0.01 units → light frustum shifts → shadow pattern shifts → flickering.

With snapping: Forces light frustum XY boundaries to align to shadow texel grid. Camera moves → frustum snaps to nearest texel boundary → shadows stable.

---

## Section 4: Shadow Atlas (Texture2DArray Storage)

### 4.1 Vulkan Image Layout

```
Texture2DArray (4 layers, 1024x1024 each)

Layer 0 (Cascade 0 - Near)     1024x1024
Layer 1 (Cascade 1)            1024x1024
Layer 2 (Cascade 2)            1024x1024
Layer 3 (Cascade 3 - Far)      1024x1024

Total: ~16 MB (D32_SFLOAT)
```

### 4.2 Texture Creation

```cpp
const VkImageCreateInfo cascadeArrayInfo{
  .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
  .imageType = VK_IMAGE_TYPE_2D,
  .format = VK_FORMAT_D32_SFLOAT,
  .extent = {1024, 1024, 1},
  .mipLevels = 1,
  .arrayLayers = 4,  // 4 layers for 4 cascades
  .samples = VK_SAMPLE_COUNT_1_BIT,
  .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
};
```

### 4.3 View Types

- **Array View** (`VK_IMAGE_VIEW_TYPE_2D_ARRAY`): Full 4-layer view for shader sampling (single descriptor)
- **Layer Views** (`VK_IMAGE_VIEW_TYPE_2D`): Per-layer views for rendering each cascade separately

---

## Section 5: Cascade Selection & Shader Sampling

### 5.1 Cascade Selection Logic

```cpp
int selectCascadeIndex(float viewDepth)
{
  int cascadeIndex = 0;

  // cascadeSplitDistances is vec4: x=c0, y=c1, z=c2, w=c3
  float splits[4] = {
    shadow.cascadeSplitDistances.x,
    shadow.cascadeSplitDistances.y,
    shadow.cascadeSplitDistances.z,
    shadow.cascadeSplitDistances.w
  };

  [unroll]
  for(int i = 0; i < LCascadeCount - 1; ++i)
  {
    if(viewDepth > splits[i])
      cascadeIndex = i + 1;
  }

  return cascadeIndex;
}
```

### 5.2 Adaptive Bias (Cascade-scaled)

```cpp
// Base bias + cascade scaling: bias[i] = baseBias * (1.0 + i * 0.5)
float cascadeBias = baseConstantBias * (1.0 + cascadeIndex * 0.5);
float slopeBias = baseSlopeBias * (1.0 + cascadeIndex * 0.5) * slope;
float bias = cascadeBias + slopeBias;
```

### 5.3 Shadow Sampling with 3x3 PCF

```cpp
float sampleCSMShadow(float3 worldPos, float3 worldNormal, float viewDepth)
{
  int cascadeIndex = selectCascadeIndex(viewDepth);

  float4 shadowClip = mul(shadow.cascadeWorldToShadowTexture[cascadeIndex], float4(worldPos, 1.0));
  float3 shadowNdc = shadowClip.xyz / shadowClip.w;
  float2 shadowUv = shadowNdc.xy * 0.5 + 0.5;

  // Bounds check
  if(shadowUv.x < 0.0 || shadowUv.x > 1.0 || shadowUv.y < 0.0 || shadowUv.y > 1.0)
    return 1.0;
  if(shadowNdc.z < 0.0 || shadowNdc.z > 1.0)
    return 1.0;

  // 3x3 PCF
  float visibility = 0.0;
  for(int y = -1; y <= 1; ++y)
  {
    for(int x = -1; x <= 1; ++x)
    {
      float2 offset = float2(x, y) * texelSize;
      float3 sampleCoord = float3(shadowUv + offset, cascadeIndex);
      float shadowDepth = shadowMapArray.Sample(shadowSampler, sampleCoord).r;
      visibility += (shadowNdc.z + bias >= shadowDepth) ? 1.0 : 0.0;
    }
  }

  return visibility / 9.0;
}
```

---

## Section 6: ShadowPass Rendering (Per-Cascade Depth)

### 6.1 CSMShadowPass Execute Flow

```
CSMShadowPass execute():
│
│  renderCascadeLayer(0)
│    beginRenderPass (layer 0 view)
│    clear depth = 0.0
│    bind cascade 0 viewProjection + bias
│    draw meshes
│    endRenderPass
│
│  renderCascadeLayer(1)
│    ... (same pattern)
│
│  renderCascadeLayer(2)
│    ...
│
│  renderCascadeLayer(3)
│    ...
│
│  transitionTexture: DepthStencil → General
│
└─ endEvent()
```

### 6.2 Per-Cascade Camera Upload

```cpp
shaderio::CameraUniforms cascadeCamera{};
cascadeCamera.viewProjection = shadowData->cascadeViewProjection[cascadeIndex];

// Apply cascade-specific bias scaling
const float cascadeBiasScale = 1.0f + cascadeIndex * 0.5f;
cascadeCamera.shadowConstantBias = kBaseConstantBias * cascadeBiasScale;
cascadeCamera.shadowDirectionAndSlopeBias.w = kBaseSlopeBias * cascadeBiasScale;
```

---

## Section 7: Debug Visualization

### 7.1 Cascade Frustum Wireframes

Color-coded 3D wireframes showing each cascade's light view frustum:
- Cascade 0: Red (Near)
- Cascade 1: Green (Mid)
- Cascade 2: Blue (Mid-Far)
- Cascade 3: Cyan (Far)

### 7.2 Screen-Space Cascade Overlay

Color-tinted regions showing which cascade affects each pixel:
- Overlay shader reads GBuffer depth
- Reconstructs view depth
- Selects cascade
- Outputs colored overlay (25% alpha blend)

### 7.3 ImGui CSM Debug Panel

```cpp
void drawCSMDebugPanel(DebugSettings& settings, const ShadowUniforms* shadowData)
{
  ImGui::Checkbox("Show Cascade Frustums", &settings.showShadowCascades);

  if(settings.showShadowCascades)
  {
    ImGui::Combo("Cascade Filter", &settings.cascadeIndex, cascadeNames, 5);
    ImGui::Checkbox("Cascade Overlay (Screen)", &settings.cascadeOverlayMode);
  }

  // Display split distances
  if(shadowData)
  {
    ImGui::Text("Split Distances:");
    ImGui::Text("  C0: %.2f", shadowData->cascadeSplitDistances.x);
    ImGui::Text("  C1: %.2f", shadowData->cascadeSplitDistances.y);
    ImGui::Text("  C2: %.2f", shadowData->cascadeSplitDistances.z);
    ImGui::Text("  C3: %.2f", shadowData->cascadeSplitDistances.w);
  }
}
```

---

## File Structure Changes

```
New Files:
- render/CSMShadowResources.h
- render/CSMShadowResources.cpp
- render/passes/CSMShadowPass.h
- render/passes/CSMShadowPass.cpp

Modified Files:
- render/Renderer.h              (replace ShadowResources with CSMShadowResources)
- render/Renderer.cpp            (CSM init, destroy, update)
- shaders/shader_io.h            (ShadowUniforms struct, CSM constants)
- shaders/shader.shadow.slang    (cascade-aware depth shader)
- shaders/shader.light.slang     (CSM sampling function)
- shaders/shader.debug.slang     (cascade overlay shader)
- render/passes/DebugPass.cpp    (CSM debug primitives)

Removed Files:
- render/ShadowResources.h       (replaced by CSMShadowResources)
- render/ShadowResources.cpp
- render/passes/ShadowPass.h     (replaced by CSMShadowPass)
- render/passes/ShadowPass.cpp
```

---

## Success Criteria

1. **Cascade Rendering**: All 4 cascades render correctly with proper depth content
2. **Cascade Selection**: Pixels sample correct cascade based on view depth
3. **Shadow Quality**: Near shadows sharp (cascade 0), far shadows acceptable (cascade 3)
4. **Stabilization**: No flickering during camera translation or rotation
5. **Adaptive Bias**: No acne on near cascades, no peter-panning on far cascades
6. **Debug Visualization**: Frustum wireframes and overlay work correctly
7. **Performance**: Acceptable frame rate impact (4x shadow renders)
8. **Memory**: ~16MB GPU memory for shadow atlas

---

## Performance Considerations

| Metric | Impact |
|--------|--------|
| Shadow Pass Time | 4x increase (4 cascade renders) |
| LightPass Sampling | Minimal (cascade selection + 1 texture sample) |
| GPU Memory | +16MB (1024x1024x4 D32) |
| Descriptor Bindings | Same (single Texture2DArray) |

---

## Migration Notes

- Complete rewrite of shadow system (Approach A)
- No legacy shadow fallback during migration
- Shader and C++ changes must be synchronized
- Test stabilization early (sphere + texel snap critical)