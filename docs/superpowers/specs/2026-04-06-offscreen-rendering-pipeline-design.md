# 离屏渲染管线重构设计

## 概述

将渲染管线从直接渲染到 swapchain 重构为离屏渲染架构，支持固定分辨率输出和宽高比保持。

## 当前架构问题

1. **LightPass 直接渲染到 swapchain** - 与 ImGui 渲染冲突
2. **ImGui viewport 显示错误** - 显示 GBuffer[0] 而非最终 PBR 结果
3. **缺少输出缓冲** - 无法进行后处理或分辨率独立

## 目标架构

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  GBufferPass │────▶│  LightPass  │────▶│ ForwardPass │────▶│ PresentPass │────▶│  ImguiPass  │
└─────────────┘     └─────────────┘     └─────────────┘     └─────────────┘     └─────────────┘
       │                   │                   │                   │                   │
       ▼                   ▼                   ▼                   ▼                   ▼
  GBuffer[0-2]        OutputTexture       OutputTexture        Swapchain          Swapchain
  (BaseColor,         (1920x1080,         (blended)            (blit +            (UI overlay)
   Normal,             sRGB)                                   letterbox)
   Material)
```

## 资源定义

### OutputTexture (离屏渲染目标)

| 属性 | 值 |
|------|-----|
| 分辨率 | 1920 x 1080 (固定) |
| 格式 | VK_FORMAT_B8G8R8A8_SRGB |
| 用途 | Color Attachment + Sampled |
| 位置 | SceneResources, 索引 3 |

### GBuffer 纹理 (保持不变)

| 索引 | 内容 | 格式 |
|------|------|------|
| 0 | BaseColor.rgb | R8G8B8A8_UNORM |
| 1 | Normal (编码 [0,1]) | R8G8B8A8_UNORM |
| 2 | Material (M, R, AO) | R8G8B8A8_UNORM |

## Pass 详细设计

### 1. GBufferPass (无改动)

- **输入**: VertexBuffer, MaterialTextures
- **输出**: GBuffer[0-2], Depth
- **依赖**: 无

### 2. LightPass (修改)

**变更内容:**
- 渲染目标从 swapchain 改为 OutputTexture
- 清空颜色为黑色 (0,0,0,1)

**资源依赖:**
- 读: GBuffer[0-2], Depth, CameraUniforms
- 写: OutputTexture

**关键代码改动:**
```cpp
// LightPass.cpp
rhi::RenderTargetDesc colorTarget = {
    .view = rhi::TextureViewHandle::fromNative(
        m_renderer->getOutputTextureView()),  // 改为离屏 RT
    .loadOp = rhi::LoadOp::clear,
    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
};
```

### 3. ForwardPass (修改)

**变更内容:**
- 渲染目标从 swapchain 改为 OutputTexture
- 使用 LoadOp::load 保留 LightPass 结果

**资源依赖:**
- 读: Depth, MaterialTextures, CameraUniforms
- 读写: OutputTexture

### 4. PresentPass (重构)

**变更内容:**
- 不再只是 beginRendering
- 执行 vkCmdBlitImage 从 OutputTexture 到 Swapchain
- 保持 16:9 宽高比，黑边填充

**Blit 逻辑:**
```cpp
// 计算 letterbox 区域
float rtAspect = 1920.0f / 1080.0f;  // 16:9
float swapAspect = (float)swapchainWidth / swapchainHeight;

VkRect2D dstRect;
if (swapAspect > rtAspect) {
    // Swapchain 更宽 -> pillarbox (左右黑边)
    dstRect.extent.width = swapchainHeight * rtAspect;
    dstRect.extent.height = swapchainHeight;
    dstRect.offset.x = (swapchainWidth - dstRect.extent.width) / 2;
    dstRect.offset.y = 0;
} else {
    // Swapchain 更高 -> letterbox (上下黑边)
    dstRect.extent.width = swapchainWidth;
    dstRect.extent.height = swapchainWidth / rtAspect;
    dstRect.offset.x = 0;
    dstRect.offset.y = (swapchainHeight - dstRect.extent.height) / 2;
}
```

### 5. ImguiPass (无改动)

- 继续渲染到 swapchain
- 在 PresentPass 之后执行

## SceneResources 改动

**新增成员:**
```cpp
// 固定分辨率输出纹理
static constexpr uint32_t kOutputTextureWidth = 1920;
static constexpr uint32_t kOutputTextureHeight = 1080;
static constexpr uint32_t kOutputTextureIndex = 3;  // GBuffer[0-2] 之后

VkImageView getOutputTextureView() const;
ImTextureID getOutputTextureImID() const;
```

**纹理创建:**
```cpp
// create() 中新增
VkImageCreateInfo outputInfo{
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = VK_FORMAT_B8G8R8A8_SRGB,
    .extent = {kOutputTextureWidth, kOutputTextureHeight, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT 
           | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
};
```

## Renderer 改动

**新增接口:**
```cpp
VkImageView getOutputTextureView() const;
ImTextureID getOutputTextureID() const;
```

**viewportAttachmentIndex 更新:**
```cpp
// 从 0 (GBuffer[0]) 改为 3 (OutputTexture)
m_materials.viewportTextureHandle = m_materials.texturePool.emplace({
    .hot = {
        .runtimeKind = TextureRuntimeKind::viewportAttachment,
        .viewportAttachmentIndex = 3,  // OutputTexture
    },
});
```

## Pass 执行顺序

```cpp
// Renderer.cpp init()
m_passExecutor.clear();
m_passExecutor.addPass(*m_gbufferPass);
m_passExecutor.addPass(*m_lightPass);
m_passExecutor.addPass(*m_forwardPass);
m_passExecutor.addPass(*m_presentPass);
m_passExecutor.addPass(*m_imguiPass);
```

## 资源依赖声明

```cpp
// LightPass::getDependencies()
PassResourceDependency::texture(kPassGBufferColorHandle, ResourceAccess::read, ...),
PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::write, ...),

// ForwardPass::getDependencies()  
PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::readWrite, ...),
PassResourceDependency::texture(kPassGBufferDepthHandle, ResourceAccess::read, ...),

// PresentPass::getDependencies()
PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::read, ...),
PassResourceDependency::texture(kPassSwapchainHandle, ResourceAccess::write, ...),
```

## 新增 Handle 常量

```cpp
// Pass.h
inline constexpr TextureHandle kPassOutputHandle{3, 1};  // OutputTexture
```

## 文件修改清单

| 文件 | 改动类型 | 描述 |
|------|----------|------|
| render/SceneResources.h | 修改 | 新增 OutputTexture 相关接口 |
| render/SceneResources.cpp | 修改 | 创建/销毁 OutputTexture |
| render/Renderer.h | 修改 | 新增 getOutputTextureView 接口 |
| render/Renderer.cpp | 修改 | viewportAttachmentIndex 更新 |
| render/passes/LightPass.cpp | 修改 | 渲染到 OutputTexture |
| render/passes/ForwardPass.cpp | 修改 | 渲染到 OutputTexture |
| render/passes/PresentPass.cpp | 重构 | blit OutputTexture 到 swapchain |
| render/Pass.h | 修改 | 新增 kPassOutputHandle |

## 测试计划

1. **基础功能** - 确认 GBufferPass → LightPass → PresentPass 流程正常
2. **宽高比测试** - 测试不同窗口比例下的 letterbox/pillarbox 效果
3. **ImGui viewport** - 确认显示的是最终 PBR 结果而非 GBuffer
4. **透明物体** - 确认 ForwardPass 正确混合到 OutputTexture

## 风险与缓解

| 风险 | 缓解措施 |
|------|----------|
| sRGB 格式与 shader gamma 处理冲突 | 验证最终颜色输出正确 |
| blit 性能开销 | vkCmdBlitImage 通常高效，可后续优化为 vkCmdCopyImage |
| 固定分辨率可能模糊 | 这是预期行为，可后续添加超采样选项 |