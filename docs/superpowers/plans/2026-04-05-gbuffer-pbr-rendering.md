# GBuffer PBR Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 GBuffer MRT 渲染 + 完整 PBR 材质支持，LightPass 输出 BaseColor

**Architecture:** 分层实现：Layer 1 (基础设施: GBuffer MRT + MaterialPool) → Layer 2 (GBufferPass shader) → Layer 3 (LightPass 修复) → Layer 4 (验证)

**Tech Stack:** Vulkan, Slang shaders, tinygltf, glm

---

## File Structure

```
Files to CREATE:
  shaders/shader.gbuffer.slang     - GBuffer MRT output shader

Files to MODIFY:
  render/SceneResources.h          - Add GBuffer MRT interface (3 color targets)
  render/SceneResources.cpp        - Implement GBuffer MRT creation
  loader/GltfLoader.h              - Extend GltfMaterialData with PBR fields
  loader/GltfLoader.cpp            - Read PBR attributes, generate tangents
  render/Renderer.h                - Extend MaterialRecord with PBR texture handles
  render/Renderer.cpp              - Upload PBR textures, update material creation
  render/passes/GBufferPass.cpp    - Use GBuffer shader, pass PBR params
  render/passes/LightPass.cpp      - Add render pass, bind GBuffer textures
  shaders/shader_io.h              - Add GBuffer push constant structures
  shaders/shader.light.slang       - Reserve GBuffer texture bindings
  render/MeshPool.cpp              - Support tangent in vertex upload
  render/MeshPool.h                - Add tangent to MeshRecord
  common/Common.h                  - Add Normal packing utilities
```

---

## Layer 1: Infrastructure

### Task 1: Normal Packing Utilities

**Files:**
- Modify: `common/Common.h`

- [ ] **Step 1: Add pack/unpack functions to Common.h**

```cpp
// Add after existing utilities in common/Common.h

// Normal packing for RGB10A2 format
inline uint32_t packNormalRGB10A2(const glm::vec3& normal) {
    const uint32_t x = static_cast<uint32_t>(glm::clamp(normal.x * 0.5f + 0.5f, 0.0f, 1.0f) * 1023.0f);
    const uint32_t y = static_cast<uint32_t>(glm::clamp(normal.y * 0.5f + 0.5f, 0.0f, 1.0f) * 1023.0f);
    const uint32_t z = static_cast<uint32_t>(glm::clamp(normal.z * 0.5f + 0.5f, 0.0f, 1.0f) * 1023.0f);
    return (z << 20) | (y << 10) | x;
}

inline glm::vec3 unpackNormalRGB10A2(uint32_t packed) {
    const float x = static_cast<float>(packed & 0x3FF) / 1023.0f * 2.0f - 1.0f;
    const float y = static_cast<float>((packed >> 10) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
    const float z = static_cast<float>((packed >> 20) & 0x3FF) / 1023.0f * 2.0f - 1.0f;
    return glm::normalize(glm::vec3(x, y, z));
}
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add common/Common.h
git commit -m "feat(common): add RGB10A2 normal packing utilities"
```

---

### Task 2: Extend GltfMaterialData for PBR

**Files:**
- Modify: `loader/GltfLoader.h`

- [ ] **Step 1: Extend GltfMaterialData structure**

Replace the existing `GltfMaterialData` struct in `loader/GltfLoader.h`:

```cpp
struct GltfMaterialData {
    // Base color
    int baseColorTexture = -1;         // -1 = no texture
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    
    // Metallic-Roughness (glTF convention: G=metallic, B=roughness)
    int metallicRoughnessTexture = -1;
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    
    // Normal map
    int normalTexture = -1;
    float normalScale = 1.0f;
    
    // Occlusion
    int occlusionTexture = -1;
    float occlusionStrength = 1.0f;
    
    // Emissive
    int emissiveTexture = -1;
    glm::vec3 emissiveFactor = glm::vec3(0.0f);
    
    // Additional flags
    bool doubleSided = false;
    std::string name;
};
```

- [ ] **Step 2: Add tangents to GltfMeshData**

Replace the existing `GltfMeshData` struct:

```cpp
struct GltfMeshData {
    std::vector<float> positions;      // xyz (12 bytes per vertex)
    std::vector<float> normals;        // xyz (12 bytes)
    std::vector<float> texCoords;      // uv (8 bytes)
    std::vector<float> tangents;       // xyzw (16 bytes, auto-generated if missing)
    std::vector<uint32_t> indices;
    int materialIndex = -1;
    glm::mat4 transform = glm::mat4(1.0f);
};
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add loader/GltfLoader.h
git commit -m "feat(loader): extend GltfMaterialData for full PBR support"
```

---

### Task 3: Read PBR Attributes in GltfLoader

**Files:**
- Modify: `loader/GltfLoader.cpp`

- [ ] **Step 1: Update processMaterials function**

Replace the existing `processMaterials` function in `loader/GltfLoader.cpp`:

```cpp
void GltfLoader::processMaterials(const tinygltf::Model& model, GltfModel& outModel) {
    outModel.materials.reserve(model.materials.size());

    for (const auto& mat : model.materials) {
        GltfMaterialData data;
        data.name = mat.name;
        data.doubleSided = mat.doubleSided;
        
        // PBR Metallic-Roughness
        const auto& pbr = mat.pbrMetallicRoughness;
        
        // Base color
        data.baseColorFactor = glm::vec4(
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3])
        );
        if (pbr.baseColorTexture.index >= 0) {
            data.baseColorTexture = pbr.baseColorTexture.index;
        }
        
        // Metallic-Roughness
        data.metallicFactor = static_cast<float>(pbr.metallicFactor);
        data.roughnessFactor = static_cast<float>(pbr.roughnessFactor);
        if (pbr.metallicRoughnessTexture.index >= 0) {
            data.metallicRoughnessTexture = pbr.metallicRoughnessTexture.index;
        }
        
        // Normal map
        if (mat.normalTexture.index >= 0) {
            data.normalTexture = mat.normalTexture.index;
            data.normalScale = static_cast<float>(mat.normalTexture.scale);
        }
        
        // Occlusion
        if (mat.occlusionTexture.index >= 0) {
            data.occlusionTexture = mat.occlusionTexture.index;
            data.occlusionStrength = static_cast<float>(mat.occlusionTexture.strength);
        }
        
        // Emissive
        data.emissiveFactor = glm::vec3(
            static_cast<float>(mat.emissiveFactor[0]),
            static_cast<float>(mat.emissiveFactor[1]),
            static_cast<float>(mat.emissiveFactor[2])
        );
        if (mat.emissiveTexture.index >= 0) {
            data.emissiveTexture = mat.emissiveTexture.index;
        }
        
        outModel.materials.push_back(data);
    }
}
```

- [ ] **Step 2: Add tangent generation helper function**

Add after the `processImages` function in `loader/GltfLoader.cpp`:

```cpp
std::vector<float> computeTangents(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<float>& texCoords,
    const std::vector<uint32_t>& indices
) {
    const size_t vertexCount = positions.size() / 3;
    std::vector<glm::vec3> tangents(vertexCount, glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(vertexCount, glm::vec3(0.0f));
    
    // Accumulate tangent vectors for each triangle
    for (size_t i = 0; i < indices.size(); i += 3) {
        const uint32_t i0 = indices[i];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        
        const glm::vec3 p0(positions[i0 * 3], positions[i0 * 3 + 1], positions[i0 * 3 + 2]);
        const glm::vec3 p1(positions[i1 * 3], positions[i1 * 3 + 1], positions[i1 * 3 + 2]);
        const glm::vec3 p2(positions[i2 * 3], positions[i2 * 3 + 1], positions[i2 * 3 + 2]);
        
        const glm::vec2 uv0(texCoords[i0 * 2], texCoords[i0 * 2 + 1]);
        const glm::vec2 uv1(texCoords[i1 * 2], texCoords[i1 * 2 + 1]);
        const glm::vec2 uv2(texCoords[i2 * 2], texCoords[i2 * 2 + 1]);
        
        const glm::vec3 e1 = p1 - p0;
        const glm::vec3 e2 = p2 - p0;
        const glm::vec2 duv1 = uv1 - uv0;
        const glm::vec2 duv2 = uv2 - uv0;
        
        const float r = 1.0f / (duv1.x * duv2.y - duv2.x * duv1.y);
        
        const glm::vec3 tangent = r * (e1 * duv2.y - e2 * duv1.y);
        const glm::vec3 bitangent = r * (e2 * duv1.x - e1 * duv2.x);
        
        tangents[i0] += tangent;
        tangents[i1] += tangent;
        tangents[i2] += tangent;
        
        bitangents[i0] += bitangent;
        bitangents[i1] += bitangent;
        bitangents[i2] += bitangent;
    }
    
    // Orthogonalize and compute handedness
    std::vector<float> result;
    result.reserve(vertexCount * 4);
    
    for (size_t i = 0; i < vertexCount; ++i) {
        const glm::vec3 n(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]);
        glm::vec3 t = tangents[i] - n * glm::dot(n, tangents[i]);
        t = glm::normalize(t);
        
        // Handedness (w component)
        const float w = (glm::dot(glm::cross(n, t), bitangents[i]) < 0.0f) ? -1.0f : 1.0f;
        
        result.push_back(t.x);
        result.push_back(t.y);
        result.push_back(t.z);
        result.push_back(w);
    }
    
    return result;
}

void generateTangentsIfMissing(GltfMeshData& mesh) {
    if (mesh.tangents.empty() && !mesh.normals.empty() && !mesh.texCoords.empty()) {
        mesh.tangents = computeTangents(mesh.positions, mesh.normals, mesh.texCoords, mesh.indices);
    }
}
```

- [ ] **Step 3: Call tangent generation in processMesh**

Add at the end of the `processMesh` function, just before `outModel.meshes.push_back(std::move(meshData));`:

```cpp
        // Generate tangents if not provided
        generateTangentsIfMissing(meshData);

        outModel.meshes.push_back(std::move(meshData));
```

- [ ] **Step 4: Add include for numeric limits**

Add at the top of `loader/GltfLoader.cpp` if not present:

```cpp
#include <cmath>
```

- [ ] **Step 5: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds

- [ ] **Step 6: Commit**

```bash
git add loader/GltfLoader.cpp
git commit -m "feat(loader): read full PBR attributes and generate tangents"
```

---

### Task 4: Extend SceneResources for GBuffer MRT

**Files:**
- Modify: `render/SceneResources.h`
- Modify: `render/SceneResources.cpp`

- [ ] **Step 1: Add GBuffer accessor methods to SceneResources.h**

Add after `getDepthImageView()` declaration:

```cpp
    [[nodiscard]] VkImageView                  getGBufferImageView(uint32_t index) const;
    [[nodiscard]] const VkDescriptorImageInfo& getGBufferDescriptor(uint32_t index) const;
```

- [ ] **Step 2: Implement GBuffer accessors in SceneResources.cpp**

Add after `getDepthImageView()` implementation:

```cpp
VkImageView SceneResources::getGBufferImageView(uint32_t index) const
{
    return m_resources.descriptors[index].imageView;
}

const VkDescriptorImageInfo& SceneResources::getGBufferDescriptor(uint32_t index) const
{
    return m_resources.descriptors[index];
}
```

- [ ] **Step 3: Note on MRT Creation**

The existing `SceneResources::create()` already supports multiple color attachments via the `CreateInfo::color` vector. The Renderer initialization code needs to pass 3 color formats:

```cpp
SceneResources::CreateInfo sceneInfo{
    .size = m_swapchainDependent.windowSize,
    .color = {
        VK_FORMAT_R8G8B8A8_UNORM,              // GBuffer0: BaseColor
        VK_FORMAT_A2R10G10B10_UNORM_PACK32,    // GBuffer1: Normal (if supported)
        VK_FORMAT_R8G8B8A8_UNORM,              // GBuffer2: MaterialParams
    },
    .depth = VK_FORMAT_D32_SFLOAT,
    .linearSampler = m_device.linearSampler,
    .sampleCount = VK_SAMPLE_COUNT_1_BIT,
};
```

This modification will be done in Task 12 (Verification).

- [ ] **Step 4: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
git add render/SceneResources.h render/SceneResources.cpp
git commit -m "feat(render): add GBuffer accessor methods to SceneResources"
```

---

### Task 5: Extend MaterialRecord for PBR Textures

**Files:**
- Modify: `render/Renderer.h`

- [ ] **Step 1: Extend MaterialRecord in Renderer.h**

Find the `MaterialRecord` struct inside `MaterialResources` and replace it with:

```cpp
    struct MaterialRecord
    {
      // PBR Texture handles (each independent for sharing)
      TextureHandle baseColorTexture{kNullTextureHandle};
      TextureHandle metallicRoughnessTexture{kNullTextureHandle};
      TextureHandle normalTexture{kNullTextureHandle};
      TextureHandle occlusionTexture{kNullTextureHandle};
      TextureHandle emissiveTexture{kNullTextureHandle};
      
      // Texture view for descriptor (legacy compatibility)
      TextureHandle      sampledTexture{};
      
      // PBR Factors (fallback when texture missing)
      glm::vec4 baseColorFactor{1.0f};
      float     metallicFactor{1.0f};
      float     roughnessFactor{1.0f};
      float     normalScale{1.0f};
      float     occlusionStrength{1.0f};
      glm::vec3 emissiveFactor{0.0f};
      
      // Bindless descriptor slot
      rhi::ResourceIndex descriptorIndex{0};
      const char*        debugName{"material"};
    };
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds (may have unused variable warnings)

- [ ] **Step 3: Commit**

```bash
git add render/Renderer.h
git commit -m "feat(render): extend MaterialRecord for PBR texture handles"
```

---

### Task 6: Update MeshPool for Tangent Support

**Files:**
- Modify: `render/MeshPool.cpp`

- [ ] **Step 1: Update vertex buffer stride for tangent**

In `MeshPool::uploadMesh`, change the vertex stride from 32 to 48 bytes:

```cpp
    // Build interleaved vertex buffer: Position(12) + Normal(12) + TexCoord(8) + Tangent(16) = 48 bytes
    std::vector<uint8_t> vertexData(record.vertexCount * 48);

    for (uint32_t i = 0; i < record.vertexCount; ++i) {
        float* dst = reinterpret_cast<float*>(&vertexData[i * 48]);

        // Position
        dst[0] = meshData.positions[i * 3 + 0];
        dst[1] = meshData.positions[i * 3 + 1];
        dst[2] = meshData.positions[i * 3 + 2];

        // Normal
        if (!meshData.normals.empty()) {
            dst[3] = meshData.normals[i * 3 + 0];
            dst[4] = meshData.normals[i * 3 + 1];
            dst[5] = meshData.normals[i * 3 + 2];
        } else {
            dst[3] = 0.0f;
            dst[4] = 1.0f;
            dst[5] = 0.0f;
        }

        // TexCoord
        if (!meshData.texCoords.empty()) {
            dst[6] = meshData.texCoords[i * 2 + 0];
            dst[7] = meshData.texCoords[i * 2 + 1];
        } else {
            dst[6] = 0.0f;
            dst[7] = 0.0f;
        }
        
        // Tangent
        if (!meshData.tangents.empty()) {
            dst[8] = meshData.tangents[i * 4 + 0];
            dst[9] = meshData.tangents[i * 4 + 1];
            dst[10] = meshData.tangents[i * 4 + 2];
            dst[11] = meshData.tangents[i * 4 + 3];
        } else {
            // Default tangent (right-handed)
            dst[8] = 1.0f;
            dst[9] = 0.0f;
            dst[10] = 0.0f;
            dst[11] = 1.0f;
        }
    }
```

- [ ] **Step 2: Update vertexStride in MeshRecord**

Add `vertexStride` field initialization:

```cpp
    record.vertexStride = 48;  // Position(12) + Normal(12) + TexCoord(8) + Tangent(16)
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add render/MeshPool.cpp
git commit -m "feat(render): add tangent support to MeshPool vertex upload"
```

---

### Task 7: Update uploadGltfModel for PBR Materials

**Files:**
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Update material creation in uploadGltfModel**

Find the material creation loop in `uploadGltfModel` and replace it with:

```cpp
  // Create materials with PBR properties
  for(const auto& matData : model.materials)
  {
    MaterialResources::MaterialRecord record;
    
    // Base color texture
    if(matData.baseColorTexture >= 0 && matData.baseColorTexture < static_cast<int>(result.textures.size()))
    {
      record.baseColorTexture = result.textures[matData.baseColorTexture];
      record.sampledTexture = record.baseColorTexture;  // Legacy compatibility
    }
    
    // Metallic-Roughness texture
    if(matData.metallicRoughnessTexture >= 0 && matData.metallicRoughnessTexture < static_cast<int>(result.textures.size()))
    {
      record.metallicRoughnessTexture = result.textures[matData.metallicRoughnessTexture];
    }
    
    // Normal texture
    if(matData.normalTexture >= 0 && matData.normalTexture < static_cast<int>(result.textures.size()))
    {
      record.normalTexture = result.textures[matData.normalTexture];
    }
    
    // Occlusion texture
    if(matData.occlusionTexture >= 0 && matData.occlusionTexture < static_cast<int>(result.textures.size()))
    {
      record.occlusionTexture = result.textures[matData.occlusionTexture];
    }
    
    // Emissive texture
    if(matData.emissiveTexture >= 0 && matData.emissiveTexture < static_cast<int>(result.textures.size()))
    {
      record.emissiveTexture = result.textures[matData.emissiveTexture];
    }
    
    // Factors
    record.baseColorFactor = matData.baseColorFactor;
    record.metallicFactor = matData.metallicFactor;
    record.roughnessFactor = matData.roughnessFactor;
    record.normalScale = matData.normalScale;
    record.occlusionStrength = matData.occlusionStrength;
    record.emissiveFactor = matData.emissiveFactor;
    
    record.descriptorIndex = static_cast<rhi::ResourceIndex>(result.materials.size());
    record.debugName = matData.name.empty() ? "gltf-material" : matData.name.c_str();
    
    MaterialHandle matHandle = m_materials.materialPool.emplace(std::move(record));
    result.materials.push_back(matHandle);
  }
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add render/Renderer.cpp
git commit -m "feat(render): upload PBR material properties in uploadGltfModel"
```

---

## Layer 2: GBuffer Shader

### Task 8: Add GBuffer Shader Structures to shader_io.h

**Files:**
- Modify: `shaders/shader_io.h`

- [ ] **Step 1: Add tangent vertex location and GBuffer push constant**

Add after the existing `PushConstantGltf` struct:

```cpp
// Tangent vertex location
STATIC_CONST int LVGltfTangent = 3;

// Vertex with tangent for GBuffer pass
struct VertexGltfTangent
{
  vec3 position;
  vec3 normal;
  vec2 texCoord;
  vec4 tangent;  // xyz = tangent direction, w = handedness
};

// Push constant for GBuffer pass with PBR material params
struct PushConstantGBuffer
{
  mat4 modelMatrix;
  mat4 viewProjectionMatrix;
  
  // Material factors
  vec4 baseColorFactor;
  float metallicFactor;
  float roughnessFactor;
  float normalScale;
  float occlusionStrength;
  vec3 emissiveFactor;
  float _padding;
  
  // Texture indices (bindless)
  uint baseColorTextureIndex;
  uint metallicRoughnessTextureIndex;
  uint normalTextureIndex;
  uint occlusionTextureIndex;
  uint emissiveTextureIndex;
  
  // Flags (0/1)
  uint hasBaseColorTexture;
  uint hasMetallicRoughnessTexture;
  uint hasNormalTexture;
  uint hasOcclusionTexture;
  uint hasEmissiveTexture;
  uint _padding2[3];
};

// GBuffer output (MRT)
struct GBufferOutput
{
  vec4 GBuffer0;  // BaseColor.rgb + unused
  vec4 GBuffer1;  // Normal.xyz (packed RGB10A2)
  vec4 GBuffer2;  // M.r + R.g + AO.b + unused
};
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add shaders/shader_io.h
git commit -m "feat(shader): add GBuffer push constant and MRT structures"
```

---

### Task 9: Create GBuffer Shader

**Files:**
- Create: `shaders/shader.gbuffer.slang`

- [ ] **Step 1: Create the GBuffer shader file**

```slang
// GBuffer Pass Shader: Output PBR material properties to MRT
// Vertex format: Position(12) + Normal(12) + TexCoord(8) + Tangent(16) = 48 bytes

#include "shader_io.h"

//------------------------------------------------------------------------------
// Input Structures
//------------------------------------------------------------------------------

struct VertexInput
{
    [[vk::location(LVGltfPosition)]] float3 position : POSITION;
    [[vk::location(LVGltfNormal)]]   float3 normal   : NORMAL;
    [[vk::location(LVGltfTexCoord)]] float2 texCoord : TEXCOORD0;
    [[vk::location(LVGltfTangent)]]  float4 tangent  : TANGENT;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float3 worldTangent : TEXCOORD2;
    float tangentW : TEXCOORD3;
};

//------------------------------------------------------------------------------
// Resource Bindings
//------------------------------------------------------------------------------

[[vk::binding(LBindTextures, LSetTextures)]]
Sampler2D inTexture[];

[[vk::binding(LBindSceneInfo, LSetScene)]]
ConstantBuffer<SceneInfo> sceneInfo;

[[vk::push_constant]]
ConstantBuffer<PushConstantGBuffer> pushConst;

//------------------------------------------------------------------------------
// Vertex Shader
//------------------------------------------------------------------------------

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    
    // Transform to world space
    float4 worldPos = mul(pushConst.modelMatrix, float4(input.position, 1.0));
    output.position = mul(pushConst.viewProjectionMatrix, worldPos);
    
    // Pass UV
    output.uv = input.texCoord;
    
    // Transform normal and tangent to world space
    output.worldNormal = normalize(mul(pushConst.modelMatrix, float4(input.normal, 0.0)).xyz);
    output.worldTangent = normalize(mul(pushConst.modelMatrix, float4(input.tangent.xyz, 0.0)).xyz);
    output.tangentW = input.tangent.w;
    
    return output;
}

//------------------------------------------------------------------------------
// Fragment Shader (MRT Output)
//------------------------------------------------------------------------------

[shader("fragment")]
void fragmentMain(
    VertexOutput input,
    out float4 outGBuffer0 : SV_Target0,
    out float4 outGBuffer1 : SV_Target1,
    out float4 outGBuffer2 : SV_Target2
) {
    // 1. BaseColor
    float4 color = pushConst.baseColorFactor;
    if (pushConst.hasBaseColorTexture != 0) {
        color *= inTexture[pushConst.baseColorTextureIndex].Sample(input.uv);
    }
    
    // 2. Normal (world space, apply normal map if present)
    float3 worldNormal = normalize(input.worldNormal);
    
    if (pushConst.hasNormalTexture != 0) {
        // Sample normal map and convert from [0,1] to [-1,1]
        float3 tangentNormal = inTexture[pushConst.normalTextureIndex].Sample(input.uv).rgb;
        tangentNormal = tangentNormal * 2.0 - 1.0;
        tangentNormal.y = -tangentNormal.y;  // glTF convention: flip Y
        
        // Build TBN matrix
        float3 T = normalize(input.worldTangent);
        float3 B = normalize(cross(worldNormal, T) * input.tangentW);
        float3x3 TBN = float3x3(T, B, worldNormal);
        
        // Transform tangent normal to world space
        worldNormal = normalize(mul(TBN, tangentNormal * pushConst.normalScale));
    }
    
    // 3. Metallic-Roughness (glTF: G=metallic, B=roughness)
    float metallic = pushConst.metallicFactor;
    float roughness = pushConst.roughnessFactor;
    
    if (pushConst.hasMetallicRoughnessTexture != 0) {
        float4 mr = inTexture[pushConst.metallicRoughnessTextureIndex].Sample(input.uv);
        metallic *= mr.g;   // G channel
        roughness *= mr.b;  // B channel
    }
    
    // 4. Occlusion
    float ao = 1.0;
    if (pushConst.hasOcclusionTexture != 0) {
        ao = inTexture[pushConst.occlusionTextureIndex].Sample(input.uv).r;
        ao = lerp(1.0, ao, pushConst.occlusionStrength);
    }
    
    // Pack outputs
    outGBuffer0 = float4(color.rgb, 0.0);
    
    // Normal output: unpacked for RGBA8 (simpler, can optimize to RGB10A2 later)
    // Map [-1,1] to [0,1] for storage
    outGBuffer1 = float4(worldNormal * 0.5 + 0.5, 0.0);
    
    outGBuffer2 = float4(metallic, roughness, ao, 0.0);
}
```

- [ ] **Step 2: Build and verify shader compilation**

Run: `cmake --build build`
Expected: Build succeeds (shader compiled to SPIR-V)

- [ ] **Step 3: Commit**

```bash
git add shaders/shader.gbuffer.slang
git commit -m "feat(shader): add GBuffer MRT output shader"
```

---

## Layer 3: LightPass Fix

### Task 10: Update Light Shader with Reserved GBuffer Bindings

**Files:**
- Modify: `shaders/shader.light.slang`

- [ ] **Step 1: Update light shader with all GBuffer bindings**

Replace the entire file with:

```slang
// LightPass shader: Fullscreen triangle sampling GBuffer
// Current: Pass-through BaseColor
// Future: Full PBR lighting calculation

#ifdef USE_SLANG
#define VK_PUSH_CONSTANT [[vk::push_constant]]
#define VK_BINDING(set, binding) [[vk::binding(binding, set)]]
#else
#define VK_PUSH_CONSTANT
#define VK_BINDING(set, binding)
#endif

// GBuffer textures (all reserved for future PBR lighting)
VK_BINDING(0, 0) Texture2D gbufferBaseColor;   // GBuffer0 - USED
VK_BINDING(1, 0) Texture2D gbufferNormal;      // GBuffer1 - RESERVED
VK_BINDING(2, 0) Texture2D gbufferMaterial;    // GBuffer2 - RESERVED
VK_BINDING(3, 0) Texture2D gbufferDepth;       // Depth    - RESERVED
VK_BINDING(4, 0) SamplerState samplerLinear;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// Fullscreen triangle vertex shader
[shader("vertex")]
VSOutput vertexMain(uint vertexIndex : SV_VertexID) {
    VSOutput output;

    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };

    output.position = float4(positions[vertexIndex], 0.0, 1.0);
    output.uv = positions[vertexIndex] * 0.5 + 0.5;
    
    // Flip Y for Vulkan
    output.uv.y = 1.0 - output.uv.y;

    return output;
}

// Fragment shader: Pass-through BaseColor (current implementation)
[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target {
    // PASS-THROUGH: Output BaseColor directly
    float4 baseColor = gbufferBaseColor.Sample(samplerLinear, input.uv);
    return baseColor;
    
    // FUTURE: PBR lighting calculation
    // Normal is stored as [0,1] mapped from [-1,1]
    // float3 normal = gbufferNormal.Sample(samplerLinear, input.uv).rgb * 2.0 - 1.0;
    // float2 mr = gbufferMaterial.Sample(samplerLinear, input.uv).rg;
    // float ao = gbufferMaterial.Sample(samplerLinear, input.uv).b;
    // float3 result = computePBR(baseColor.rgb, normal, mr.x, mr.y, ao);
    // return float4(result, 1.0);
}
```

- [ ] **Step 2: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds

- [ ] **Step 3: Commit**

```bash
git add shaders/shader.light.slang
git commit -m "feat(shader): reserve all GBuffer bindings in light shader"
```

---

### Task 11: Fix LightPass Render Pass

**Files:**
- Modify: `render/passes/LightPass.cpp`

- [ ] **Step 1: Add render pass setup to LightPass::execute**

Replace the entire `execute` function in `render/passes/LightPass.cpp`:

```cpp
void LightPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr)
        return;

    context.cmd->beginEvent("LightPass");

    // Get swapchain image view and extent
    const VkImageView swapchainImageView = m_renderer->getCurrentSwapchainImageView();
    const VkExtent2D extent = m_renderer->getSwapchainExtent();
    
    if(swapchainImageView == VK_NULL_HANDLE)
    {
        context.cmd->endEvent();
        return;
    }

    // Setup color attachment for dynamic rendering
    const VkRenderingAttachmentInfo colorAttachment{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = swapchainImageView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,  // Preserve previous content
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };

    // Begin dynamic rendering (INDEPENDENT render pass)
    const VkRenderingInfo renderingInfo{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, extent},
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &colorAttachment,
    };
    rhi::vulkan::cmdBeginRendering(*context.cmd, renderingInfo);

    // Set viewport and scissor
    const VkViewport viewport{
        0.0f, 0.0f,
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        0.0f, 1.0f
    };
    const VkRect2D scissor{{0, 0}, extent};
    
    rhi::vulkan::cmdSetViewport(*context.cmd, viewport);
    rhi::vulkan::cmdSetScissor(*context.cmd, scissor);

    // Bind light pipeline
    const PipelineHandle lightPipeline = m_renderer->getLightPipelineHandle();
    if(lightPipeline.isNull())
    {
        rhi::vulkan::cmdEndRendering(*context.cmd);
        context.cmd->endEvent();
        return;
    }

    const VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(lightPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
    
    // TODO: Bind GBuffer textures when descriptor set is ready
    // bindGBufferTextures(*context.cmd);

    // Draw fullscreen triangle
    rhi::vulkan::cmdDraw(*context.cmd, 3, 1, 0, 0);

    // End dynamic rendering
    rhi::vulkan::cmdEndRendering(*context.cmd);

    context.cmd->endEvent();
}
```

- [ ] **Step 2: Add getCurrentSwapchainImageView method to Renderer.h**

Add declaration in `Renderer.h` after `getSwapchainExtent()`:

```cpp
    VkImageView getCurrentSwapchainImageView() const;
```

- [ ] **Step 3: Implement getCurrentSwapchainImageView in Renderer.cpp**

Add after `getSwapchainExtent()` implementation:

```cpp
VkImageView Renderer::getCurrentSwapchainImageView() const
{
    if(m_swapchainDependent.swapchain == nullptr)
    {
        return VK_NULL_HANDLE;
    }
    return fromNativeHandle<VkImageView>(
        m_swapchainDependent.swapchain->getNativeImageView(m_swapchainDependent.currentImageIndex));
}
```

- [ ] **Step 4: Build and verify**

Run: `cmake --build build`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
git add render/passes/LightPass.cpp render/Renderer.h render/Renderer.cpp
git commit -m "fix(render): add independent render pass to LightPass"
```

---

## Layer 4: Integration and Verification

### Task 12: Update SceneResources Initialization for MRT

**Files:**
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Find SceneResources initialization**

Search for `SceneResources::init` or `sceneResources.init` in Renderer.cpp. The exact location depends on current code structure. The initialization should be in `rebuildSwapchainDependentResources` or similar.

- [ ] **Step 2: Update CreateInfo for 3 GBuffer targets**

Find the existing `SceneResources::CreateInfo` and update it to use 3 color formats:

```cpp
SceneResources::CreateInfo sceneInfo{
    .size = m_swapchainDependent.windowSize,
    .color = {
        VK_FORMAT_R8G8B8A8_UNORM,              // GBuffer0: BaseColor
        VK_FORMAT_R8G8B8A8_UNORM,              // GBuffer1: Normal (using RGBA8 for simplicity)
        VK_FORMAT_R8G8B8A8_UNORM,              // GBuffer2: MaterialParams
    },
    .depth = VK_FORMAT_D32_SFLOAT,
    .linearSampler = m_device.linearSampler,
    .sampleCount = VK_SAMPLE_COUNT_1_BIT,
};
m_swapchainDependent.sceneResources.init(*m_device.device, m_device.allocator, cmd, sceneInfo);
```

Note: Using RGBA8 for Normal for simplicity. Can upgrade to RGB10A2 later for better precision.

- [ ] **Step 3: Build and run**

Run: `cmake --build build && ./build/bin/VKDemo.exe`
Expected: Application starts without crashes

- [ ] **Step 4: Commit**

```bash
git add render/Renderer.cpp
git commit -m "feat(render): initialize SceneResources with 3 GBuffer targets"
```

---

### Task 13: End-to-End Verification

**Files:**
- Test with existing glTF model

- [ ] **Step 1: Run application with shader_ball.gltf**

Run: `./build/bin/VKDemo.exe`
Load: `resources/shader_ball.gltf`

- [ ] **Step 2: Verify expected behavior**

Expected results:
- Model loads without errors
- GBuffer pass executes (check renderdoc/nsight if available)
- LightPass outputs something to screen (BaseColor pass-through)
- No validation errors or crashes

- [ ] **Step 3: Document current limitations**

The current implementation should show:
- BaseColor output (no lighting calculation)
- Normal maps may not work correctly (need proper tangent space)
- Metallic/Roughness values in GBuffer but not used

---

## Acceptance Criteria Checklist

- [ ] GBuffer creates 3 color attachments (BaseColor + Normal + Material) + Depth
- [ ] GltfLoader reads all PBR material attributes
- [ ] Tangents auto-generated when glTF lacks TANGENT attribute
- [ ] GBuffer shader outputs MRT correctly
- [ ] LightPass correctly manages its own render pass
- [ ] LightPass binds GBuffer textures (all slots reserved)
- [ ] Render result: model displays (BaseColor output)
- [ ] PBR lighting can be enabled later by modifying shader only (interface unchanged)

---

## Final Commit

After all tasks complete:

```bash
git add -A
git commit -m "feat(render): implement GBuffer PBR rendering with MRT

- Add GBuffer MRT (BaseColor + Normal + MaterialParams)
- Extend GltfLoader for full PBR material reading
- Auto-generate tangents when missing
- Add GBuffer shader with MRT output
- Fix LightPass with independent render pass
- Reserve all GBuffer bindings for future PBR lighting"
```