# GBuffer PBR Rendering Design

> **Date**: 2026-04-05
> **Status**: Draft
> **Author**: Claude

---

## 1. Overview

### 1.1 Goal

完善 VKDemo 的渲染架构，实现完整 GBuffer 绘制 + PBR 材质支持：
- GBuffer: MRT (BaseColor + Normal + MaterialParams + Depth)
- glTF: 读取完整 PBR 材质 (BaseColor + Metallic + Roughness + Normal + Occlusion + Emissive)
- LightPass: 修复 render pass，预留完整 PBR 接口，当前输出 BaseColor

### 1.2 Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| GBuffer Structure | 标准 PBR (MRT) | 3 color targets + depth，满足完整 PBR |
| GBuffer Formats | 紧凑打包 | RGB10A2 Normal, RGBA8 Material，节省带宽 |
| PBR Material Range | 完整 (6 textures) | BaseColor + MR + Normal + AO + Emissive |
| Material Upload | TextureHandle 分离 | 支持纹理共享，避免重复上传 |
| LightPass Strategy | 独立 render pass + 预留接口 | 接口完整，后续只需改 shader |

### 1.3 Scope

- **In Scope**:
  - GBuffer MRT 创建与管理
  - GltfLoader 读取完整 PBR 属性
  - MaterialPool 扩展 (PBR texture handles)
  - GBufferPass 新 shader (MRT output)
  - LightPass render pass 修复 + GBuffer bindings
  - Tangent 自动生成

- **Out of Scope**:
  - PBR lighting 计算 (预留接口，后续实现)
  - Skeletal animation
  - IBL (Image Based Lighting)
  - Shadows

---

## 2. Architecture

### 2.1 Module Dependency Graph

```
MinimalLatestApp
    │
    ├── GltfLoader.load()
    │       │
    │       └── GltfModel { meshes, PBR materials, images }
    │
    ├── Renderer.uploadGltfModel()
    │       │
    │       ├── MeshPool.uploadMesh() ──► MeshHandle
    │       │       └── Vertex: Position + Normal + UV + Tangent (auto-gen)
    │       │
    │       └── MaterialPool.uploadMaterial() ──► MaterialHandle
    │               └── 5 TextureHandles + PBR factors
    │
    └── Renderer.render()
            │
            ├── GBufferPass.execute()
            │       │
            │       ├── BeginDynamicRendering(GBuffer0/1/2 + Depth)
            │       ├── For each mesh:
            │       │     SetPipeline(GBufferPipeline)
            │       │     SetMaterial(PBR params via push constant)
            │       │     DrawIndexed()
            │       └── EndDynamicRendering()
            │
            ├── LightPass.execute()
            │       │
            │       ├── BeginDynamicRendering(Swapchain)
            │       ├── Bind GBuffer textures (5 slots)
            │       ├── Bind LightPipeline
            │       ├── Draw(3) ──► Fullscreen triangle
            │       └── EndDynamicRendering()
            │       │
            │       └── Output: BaseColor (pass-through)
            │
            └── PresentPass + ImguiPass
```

### 2.2 Pass Execution Order

```
Frame Rendering:
    1. GBufferPass      → GBuffer (BaseColor + Normal + Material + Depth)
    2. LightPass        → Begin render pass, fullscreen triangle → Swapchain, End render pass
    3. PresentPass      → (optional barrier)
    4. ImguiPass        → UI → Swapchain
```

### 2.3 Data Flow

```
glTF File
    │
    ▼
GltfLoader ──► GltfModel { meshes, PBR materials, images }
    │
    ▼
Renderer.uploadGltfModel()
    │
    ├─► GPU Vertex/Index Buffers (MeshPool)
    │
    └─► GPU Texture Images + Material descriptors
    │
    ▼
Per-Frame Render Loop
    │
    ├─► GBufferPass
    │       Push Constants: { MVP, PBR factors, texture indices }
    │       │
    │       ▼
    │   GBuffer Shader (MRT)
    │       │
    │       ├─► GBuffer0: BaseColor (RGBA8)
    │       ├─► GBuffer1: Normal (RGB10A2)
    │       └─► GBuffer2: Material (RGBA8: M/R/AO/E)
    │
    ▼
LightPass
    │   Bind: GBuffer0/1/2 + Depth textures
    │   │
    │   ▼
    │   Light Shader (pass-through BaseColor)
    │       │
    │       ▼
    │   Swapchain Image
```

---

## 3. GBuffer MRT Structure

### 3.1 GBuffer Targets

| Target | Format | Content |
|--------|--------|---------|
| **GBuffer0** | RGBA8 (VK_FORMAT_R8G8B8A8_UNORM) | BaseColor.rgb + unused |
| **GBuffer1** | RGB10A2 (VK_FORMAT_A2R10G10B10_UNORM_PACK32) | Normal.xyz (world space, packed) + unused |
| **GBuffer2** | RGBA8 (VK_FORMAT_R8G8B8A8_UNORM) | Metallic.r + Roughness.g + AO.b + unused |

**Emissive Handling**: Emissive 不存储在 GBuffer 中。LightPass shader 直接采样 emissiveTexture 并应用 emissiveFactor 进行计算。这避免了 GBuffer 通道不足的问题，且 emissive 通常在最终 lighting pass 才需要。
| **Depth** | D32F (VK_FORMAT_D32_SFLOAT) | Depth value |

### 3.2 SceneResources Extension

```cpp
struct GBufferCreateInfo {
    VkExtent2D size;
    std::vector<VkFormat> colorFormats = {
        VK_FORMAT_R8G8B8A8_UNORM,              // GBuffer0: BaseColor
        VK_FORMAT_A2R10G10B10_UNORM_PACK32,    // GBuffer1: Normal
        VK_FORMAT_R8G8B8A8_UNORM,              // GBuffer2: MaterialParams
    };
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
};
```

### 3.3 SceneResources Interface Extension

```cpp
class SceneResources {
public:
    // Existing methods...
    
    // New: GBuffer multi-target access
    VkImageView getGBufferImageView(uint32_t index) const;  // 0, 1, 2
    const VkDescriptorImageInfo& getGBufferDescriptor(uint32_t index) const;
    VkImageView getDepthImageView() const;  // Already exists
    
private:
    struct Resources {
        std::vector<utils::Image> gbufferImages;     // 3 color targets
        std::vector<VkImageView> gbufferViews;
        std::vector<VkDescriptorImageInfo> gbufferDescriptors;
        utils::Image depthImage;
        VkImageView depthView;
    };
};
```

---

## 4. MaterialPool Extension

### 4.1 PBR Material Data Structure

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

### 4.2 MaterialRecord (GPU-side)

```cpp
struct MaterialRecord {
    // Texture handles (each independent for sharing)
    TextureHandle baseColorTexture{kNullTextureHandle};
    TextureHandle metallicRoughnessTexture{kNullTextureHandle};
    TextureHandle normalTexture{kNullTextureHandle};
    TextureHandle occlusionTexture{kNullTextureHandle};
    TextureHandle emissiveTexture{kNullTextureHandle};
    
    // Factors (fallback when texture missing)
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    float normalScale{1.0f};
    float occlusionStrength{1.0f};
    glm::vec3 emissiveFactor{0.0f};
    
    // Bindless descriptor slot
    rhi::ResourceIndex descriptorIndex{rhi::kInvalidResourceIndex};
    
    // Debug
    std::string name{"material"};
};
```

### 4.3 Texture Upload Strategy

```cpp
// In uploadGltfModel():
std::vector<TextureHandle> textureHandles(model.images.size());

for (size_t i = 0; i < model.images.size(); ++i) {
    textureHandles[i] = uploadTexture(model.images[i], cmd);
}

// Material creation: map glTF texture index to TextureHandle
for (const auto& mat : model.materials) {
    MaterialRecord record;
    
    if (mat.baseColorTexture >= 0) {
        record.baseColorTexture = textureHandles[mat.baseColorTexture];
    }
    // ... same for other textures
    
    record.baseColorFactor = mat.baseColorFactor;
    record.metallicFactor = mat.metallicFactor;
    // ... other factors
    
    materials.push_back(createMaterial(record));
}
```

---

## 5. GBufferPass Shader

### 5.1 Vertex Input Format

```cpp
// Interleaved vertex format (40 bytes with tangent)
struct VertexGltf {
    float3 position : POSITION;   // 12 bytes
    float3 normal   : NORMAL;     // 12 bytes
    float2 texCoord : TEXCOORD0;  // 8 bytes
    float4 tangent  : TANGENT;    // 16 bytes (w = handedness)
};
```

### 5.2 Push Constants

```cpp
struct PushConstantGBuffer {
    float4x4 modelMatrix;
    float4x4 viewProjectionMatrix;
    
    // Material factors
    float4 baseColorFactor;       // rgba
    float  metallicFactor;
    float  roughnessFactor;
    float  normalScale;
    float  occlusionStrength;
    float3 emissiveFactor;
    
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
};
```

### 5.3 Fragment Output (MRT)

```cpp
struct GBufferOutput {
    float4 GBuffer0 : SV_Target0;  // BaseColor.rgb + unused
    float4 GBuffer1 : SV_Target1;  // Normal.xyz (packed RGB10A2)
    float4 GBuffer2 : SV_Target2;  // M.r + R.g + AO.b + E.a
};
```

### 5.4 Fragment Shader Logic

```slang
GBufferOutput fragmentMain(VSOutput input) {
    GBufferOutput output;
    
    // 1. BaseColor
    float4 color = pushConstant.baseColorFactor;
    if (pushConstant.hasBaseColorTexture) {
        color *= textures[pushConstant.baseColorTextureIndex].Sample(sampler, input.uv);
    }
    
    // 2. Normal (world space)
    float3 worldNormal = normalize(mul(pushConstant.modelMatrix, float4(input.normal, 0.0)).xyz);
    
    if (pushConstant.hasNormalTexture) {
        float3 tangentNormal = textures[pushConstant.normalTextureIndex].Sample(sampler, input.uv).rgb;
        tangentNormal = tangentNormal * 2.0 - 1.0;
        tangentNormal.y = -tangentNormal.y;  // glTF convention
        
        // Build TBN matrix
        float3 T = normalize(mul(pushConstant.modelMatrix, float4(input.tangent.xyz, 0.0)).xyz);
        float3 B = normalize(cross(worldNormal, T) * input.tangent.w);
        float3x3 TBN = float3x3(T, B, worldNormal);
        
        worldNormal = normalize(mul(TBN, tangentNormal * pushConstant.normalScale));
    }
    
    // 3. Metallic-Roughness (glTF: G=metallic, B=roughness)
    float metallic = pushConstant.metallicFactor;
    float roughness = pushConstant.roughnessFactor;
    
    if (pushConstant.hasMetallicRoughnessTexture) {
        float4 mr = textures[pushConstant.metallicRoughnessTextureIndex].Sample(sampler, input.uv);
        metallic *= mr.g;   // G channel
        roughness *= mr.b;  // B channel
    }
    
    // 4. Occlusion
    float ao = 1.0;
    if (pushConstant.hasOcclusionTexture) {
        ao = textures[pushConstant.occlusionTextureIndex].Sample(sampler, input.uv).r;
        ao = lerp(1.0, ao, pushConstant.occlusionStrength);
    }
    
    // Pack outputs (Emissive handled in LightPass directly)
    output.GBuffer0 = float4(color.rgb, 0.0);
    output.GBuffer1 = float4(packNormalRGB10A2(worldNormal), 0.0);
    output.GBuffer2 = float4(metallic, roughness, ao, 0.0);
    
    return output;
}
```

### 5.5 Normal Packing (RGB10A2)

```cpp
// Pack world normal [-1,1] to RGB10A2 format
uint packNormalRGB10A2(float3 normal) {
    uint x = uint(clamp(normal.x * 0.5 + 0.5, 0.0, 1.0) * 1023.0);
    uint y = uint(clamp(normal.y * 0.5 + 0.5, 0.0, 1.0) * 1023.0);
    uint z = uint(clamp(normal.z * 0.5 + 0.5, 0.0, 1.0) * 1023.0);
    return (z << 20) | (y << 10) | x;
}

// Unpack in LightPass shader
float3 unpackNormalRGB10A2(uint packed) {
    float x = (packed & 0x3FF) / 1023.0 * 2.0 - 1.0;
    float y = ((packed >> 10) & 0x3FF) / 1023.0 * 2.0 - 1.0;
    float z = ((packed >> 20) & 0x3FF) / 1023.0 * 2.0 - 1.0;
    return normalize(float3(x, y, z));
}
```

---

## 6. LightPass Fix

### 6.1 Current Problem

LightPass 没有设置 render pass，导致绘制时没有 ColorAttachment 绑定。

### 6.2 Solution: Independent Render Pass

```cpp
void LightPass::execute(const PassContext& context) const {
    context.cmd->beginEvent("LightPass");
    
    // 1. Get swapchain image view
    VkImageView swapchainView = m_renderer->getCurrentSwapchainImageView();
    VkExtent2D extent = m_renderer->getSwapchainExtent();
    
    // 2. Setup color attachment
    VkRenderingAttachmentInfo colorAttachment{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = swapchainView,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,  // Preserve previous content
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    
    // 3. Begin dynamic rendering (INDEPENDENT)
    VkRenderingInfo renderingInfo{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, extent},
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &colorAttachment,
    };
    rhi::vulkan::cmdBeginRendering(*context.cmd, renderingInfo);
    
    // 4. Set viewport/scissor
    VkViewport viewport{0.0f, 0.0f, float(extent.width), float(extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, extent};
    rhi::vulkan::cmdSetViewport(*context.cmd, viewport);
    rhi::vulkan::cmdSetScissor(*context.cmd, scissor);
    
    // 5. Bind GBuffer textures (预留所有 slots)
    bindGBufferTextures(context.cmd);
    
    // 6. Bind light pipeline
    VkPipeline pipeline = reinterpret_cast<VkPipeline>(
        m_renderer->getPipelineOpaque(m_renderer->getLightPipelineHandle(), 
                                       VK_PIPELINE_BIND_POINT_GRAPHICS));
    rhi::vulkan::cmdBindPipeline(*context.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    
    // 7. Draw fullscreen triangle
    rhi::vulkan::cmdDraw(*context.cmd, 3, 1, 0, 0);
    
    // 8. End dynamic rendering
    rhi::vulkan::cmdEndRendering(*context.cmd);
    
    context.cmd->endEvent();
}
```

### 6.3 GBuffer Texture Bindings

```cpp
void LightPass::bindGBufferTextures(rhi::CommandList& cmd) const {
    // Get GBuffer descriptors from SceneResources
    SceneResources& scene = m_renderer->getSceneResources();
    
    // Bind all GBuffer textures to light pipeline's descriptor set
    // (预留，当前只使用 GBuffer0)
    
    // Descriptor set layout:
    // [0] GBuffer0 (BaseColor) - USED
    // [1] GBuffer1 (Normal)    - RESERVED
    // [2] GBuffer2 (Material)  - RESERVED  
    // [3] Depth                 - RESERVED
    // [4] Sampler               - USED
    
    // Current implementation: only bind BaseColor + Sampler
    VkDescriptorImageInfo baseColorInfo = scene.getGBufferDescriptor(0);
    // ... update descriptor set
}
```

### 6.4 Light Shader (Pass-through + Reserved)

```slang
// shader.light.slang

[[vk::binding(0, 0)]] Texture2D gbufferBaseColor;   // GBuffer0 - USED
[[vk::binding(1, 0)]] Texture2D gbufferNormal;      // GBuffer1 - RESERVED
[[vk::binding(2, 0)]] Texture2D gbufferMaterial;    // GBuffer2 - RESERVED
[[vk::binding(3, 0)]] Texture2D gbufferDepth;       // Depth    - RESERVED
[[vk::binding(4, 0)]] SamplerState samplerLinear;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// Fullscreen triangle vertex shader
VSOutput vertexMain(uint vertexIndex : SV_VertexID) {
    VSOutput output;
    
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    
    output.position = float4(positions[vertexIndex], 0.0, 1.0);
    output.uv = positions[vertexIndex] * 0.5 + 0.5;
    
    return output;
}

// Fragment: pass-through BaseColor (current implementation)
float4 fragmentMain(VSOutput input) : SV_Target {
    // PASS-THROUGH: Output BaseColor directly
    float4 baseColor = gbufferBaseColor.Sample(samplerLinear, input.uv);
    return baseColor;
    
    // FUTURE: PBR lighting calculation
    // float3 normal = unpackNormalRGB10A2(gbufferNormal.Sample(samplerLinear, input.uv));
    // float2 mr = gbufferMaterial.Sample(samplerLinear, input.uv).rg;
    // float ao = gbufferMaterial.Sample(samplerLinear, input.uv).b;
    // 
    // Emissive: sample directly (not stored in GBuffer)
    // float3 emissive = emissiveFactor;
    // if (hasEmissiveTexture) {
    //     emissive *= emissiveTexture.Sample(samplerLinear, input.uv).rgb;
    // }
    // 
    // float3 result = computePBR(baseColor, normal, mr, ao, emissive, ...);
    // return float4(result, 1.0);
}
```

---

## 7. GltfLoader Extension

### 7.1 Extended Material Reading

```cpp
void GltfLoader::processMaterials(const tinygltf::Model& model, GltfModel& outModel) {
    for (const auto& mat : model.materials) {
        GltfMaterialData data;
        data.name = mat.name;
        data.doubleSided = mat.doubleSided;
        
        // PBR Metallic-Roughness
        const auto& pbr = mat.pbrMetallicRoughness;
        
        // Base color
        data.baseColorFactor = glm::vec4(
            pbr.baseColorFactor[0], pbr.baseColorFactor[1],
            pbr.baseColorFactor[2], pbr.baseColorFactor[3]
        );
        if (pbr.baseColorTexture.index >= 0) {
            data.baseColorTexture = pbr.baseColorTexture.index;
        }
        
        // Metallic-Roughness
        data.metallicFactor = pbr.metallicFactor;
        data.roughnessFactor = pbr.roughnessFactor;
        if (pbr.metallicRoughnessTexture.index >= 0) {
            data.metallicRoughnessTexture = pbr.metallicRoughnessTexture.index;
        }
        
        // Normal map
        if (mat.normalTexture.index >= 0) {
            data.normalTexture = mat.normalTexture.index;
            data.normalScale = mat.normalTexture.scale;
        }
        
        // Occlusion
        if (mat.occlusionTexture.index >= 0) {
            data.occlusionTexture = mat.occlusionTexture.index;
            data.occlusionStrength = mat.occlusionTexture.strength;
        }
        
        // Emissive
        data.emissiveFactor = glm::vec3(
            mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]
        );
        if (mat.emissiveTexture.index >= 0) {
            data.emissiveTexture = mat.emissiveTexture.index;
        }
        
        outModel.materials.push_back(data);
    }
}
```

### 7.2 Tangent Generation

```cpp
void GltfLoader::generateTangentsIfMissing(GltfMeshData& mesh) {
    if (mesh.tangents.empty() && !mesh.normals.empty() && !mesh.texCoords.empty()) {
        mesh.tangents = computeTangents(
            mesh.positions, mesh.normals, mesh.texCoords, mesh.indices
        );
    }
}

// Lengyel's method for tangent generation
std::vector<float> computeTangents(
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<float>& texCoords,
    const std::vector<uint32_t>& indices
) {
    // Implementation based on "Computing Tangent Space Basis Vectors"
    // by Eric Lengyel (http://www.terathon.com/code/tangent.html)
    
    size_t vertexCount = positions.size() / 3;
    std::vector<glm::vec3> tangents(vertexCount);
    std::vector<glm::vec3> bitangents(vertexCount);
    
    // Accumulate tangent vectors for each triangle
    for (size_t i = 0; i < indices.size(); i += 3) {
        uint32_t i0 = indices[i];
        uint32_t i1 = indices[i + 1];
        uint32_t i2 = indices[i + 2];
        
        glm::vec3 p0 = glm::vec3(positions[i0*3], positions[i0*3+1], positions[i0*3+2]);
        glm::vec3 p1 = glm::vec3(positions[i1*3], positions[i1*3+1], positions[i1*3+2]);
        glm::vec3 p2 = glm::vec3(positions[i2*3], positions[i2*3+1], positions[i2*3+2]);
        
        glm::vec2 uv0 = glm::vec2(texCoords[i0*2], texCoords[i0*2+1]);
        glm::vec2 uv1 = glm::vec2(texCoords[i1*2], texCoords[i1*2+1]);
        glm::vec2 uv2 = glm::vec2(texCoords[i2*2], texCoords[i2*2+1]);
        
        glm::vec3 e1 = p1 - p0;
        glm::vec3 e2 = p2 - p0;
        glm::vec2 duv1 = uv1 - uv0;
        glm::vec2 duv2 = uv2 - uv0;
        
        float r = 1.0f / (duv1.x * duv2.y - duv2.x * duv1.y);
        
        glm::vec3 tangent = r * (e1 * duv2.y - e2 * duv1.y);
        glm::vec3 bitangent = r * (e2 * duv1.x - e1 * duv2.x);
        
        tangents[i0] += tangent;
        tangents[i1] += tangent;
        tangents[i2] += tangent;
        
        bitangents[i0] += bitangent;
        bitangents[i1] += bitangent;
        bitangents[i2] += bitangent;
    }
    
    // Orthogonalize and compute handedness
    std::vector<float> result;
    for (size_t i = 0; i < vertexCount; ++i) {
        glm::vec3 n = glm::vec3(normals[i*3], normals[i*3+1], normals[i*3+2]);
        glm::vec3 t = glm::normalize(tangents[i] - n * glm::dot(n, tangents[i]));
        
        // Handedness (w component)
        float w = (glm::dot(glm::cross(n, t), bitangents[i]) < 0.0f) ? -1.0f : 1.0f;
        
        result.push_back(t.x);
        result.push_back(t.y);
        result.push_back(t.z);
        result.push_back(w);
    }
    
    return result;
}
```

### 7.3 Vertex Format with Tangent

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

---

## 8. File Changes Summary

| Operation | File | Changes |
|-----------|------|---------|
| **MODIFY** | `render/SceneResources.h/cpp` | Add MRT support (3 color targets), GBuffer accessors |
| **MODIFY** | `loader/GltfLoader.h/cpp` | Read complete PBR attributes, generate tangents |
| **MODIFY** | `render/Renderer.h/cpp` | Extend MaterialRecord, upload PBR textures |
| **MODIFY** | `render/passes/GBufferPass.cpp` | Use new GBuffer shader, pass PBR params via push constant |
| **MODIFY** | `render/passes/LightPass.cpp` | Add independent render pass, bind GBuffer textures |
| **ADD** | `shaders/shader.gbuffer.slang` | GBuffer MRT output shader |
| **MODIFY** | `shaders/shader.light.slang` | Reserve all GBuffer bindings |
| **MODIFY** | `render/DrawStreamWriter.h/cpp` | Add PBR material params to draw stream |
| **ADD** | `common/Common.h` | Normal packing utilities (pack/unpack RGB10A2) |
| **MODIFY** | `render/MeshPool.cpp` | Support tangent in vertex upload |

---

## 9. Acceptance Criteria

- [ ] GBuffer creates 3 color attachments (BaseColor + Normal + Material) + Depth
- [ ] GltfLoader reads all PBR material attributes
- [ ] Tangents auto-generated when glTF lacks TANGENT attribute
- [ ] GBuffer shader outputs MRT correctly
- [ ] LightPass correctly manages its own render pass
- [ ] LightPass binds GBuffer textures (all slots reserved)
- [ ] Render result: model displays correct BaseColor
- [ ] PBR lighting can be enabled later by modifying shader only (interface unchanged)

---

## 10. Future Extensions (Out of Scope)

These are documented for future reference:

1. **PBR Lighting Calculation**: Enable in LightPass shader
2. **IBL**: Add environment map for ambient lighting
3. **Shadows**: Shadow mapping pass
4. **Skeletal Animation**: GPU skinning in compute pass
5. **Material Variants**: Support glTF material extensions