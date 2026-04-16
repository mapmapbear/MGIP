# Offscreen Rendering Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor the rendering pipeline to render to an offscreen RT instead of directly to swapchain, enabling proper PBR output display in ImGui viewport.

**Architecture:** Add OutputTexture (1920x1080 sRGB) to SceneResources, modify LightPass/ForwardPass to render to it, refactor PresentPass to blit to swapchain with letterbox support.

**Tech Stack:** Vulkan, C++17, ImGui

---

## File Structure

| File | Responsibility |
|------|----------------|
| render/SceneResources.h | OutputTexture interface declarations |
| render/SceneResources.cpp | Create/destroy OutputTexture |
| render/Pass.h | kPassOutputHandle constant |
| render/Renderer.h | getOutputTextureView() declaration |
| render/Renderer.cpp | Bind OutputTexture to PassExecutor, update viewportAttachmentIndex |
| render/passes/LightPass.cpp | Render to OutputTexture instead of swapchain |
| render/passes/ForwardPass.cpp | Render to OutputTexture instead of swapchain |
| render/passes/PresentPass.cpp | Blit OutputTexture to swapchain with letterbox |

---

### Task 1: Add OutputTexture to SceneResources Header

**Files:**
- Modify: `render/SceneResources.h`

- [ ] **Step 1: Add constants and interface for OutputTexture**

Add the following to `SceneResources.h` after the existing constants in the public section:

```cpp
// Fixed resolution output texture (for PBR lighting result)
static constexpr uint32_t kOutputTextureWidth = 1920;
static constexpr uint32_t kOutputTextureHeight = 1080;
static constexpr uint32_t kOutputTextureIndex = 3;  // After GBuffer[0-2]

[[nodiscard]] VkImageView getOutputTextureView() const;
[[nodiscard]] ImTextureID getOutputTextureImID() const;
[[nodiscard]] VkImage getOutputTextureImage() const;
```

- [ ] **Step 2: Add member variables for OutputTexture**

Add to the private `Resources` struct after the existing members:

```cpp
VkImage outputTextureImage{};
VkImageView outputTextureView{};
ImTextureID outputTextureImID{};
```

---

### Task 2: Implement OutputTexture Creation in SceneResources

**Files:**
- Modify: `render/SceneResources.cpp`

- [ ] **Step 1: Add OutputTexture creation in create() function**

Add after the loop that creates GBuffer color images (after line ~130):

```cpp
// Create fixed-resolution output texture
{
  const VkImageCreateInfo outputInfo{
      .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType   = VK_IMAGE_TYPE_2D,
      .format      = VK_FORMAT_B8G8R8A8_SRGB,
      .extent      = {kOutputTextureWidth, kOutputTextureHeight, 1},
      .mipLevels   = 1,
      .arrayLayers = 1,
      .samples     = VK_SAMPLE_COUNT_1_BIT,
      .usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT 
                   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
  };
  m_resources.outputTextureImage = createImage(outputInfo);
  dutil.setObjectName(m_resources.outputTextureImage, "OutputTexture");

  const VkImageViewCreateInfo outputViewInfo{
      .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image            = m_resources.outputTextureImage,
      .viewType         = VK_IMAGE_VIEW_TYPE_2D,
      .format           = VK_FORMAT_B8G8R8A8_SRGB,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
  };
  VK_CHECK(vkCreateImageView(m_device, &outputViewInfo, nullptr, &m_resources.outputTextureView));
  dutil.setObjectName(m_resources.outputTextureView, "OutputTextureView");
}
```

- [ ] **Step 2: Initialize OutputTexture layout in create() function**

Add after the GBuffer layout initialization (after the loop around line ~160):

```cpp
// Initialize output texture layout
utils::cmdInitImageLayout(cmd, m_resources.outputTextureImage);
const VkClearColorValue outputClearValue = {{0.0f, 0.0f, 0.0f, 1.0f}};
const VkImageSubresourceRange outputRange{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
vkCmdClearColorImage(cmd, m_resources.outputTextureImage, VK_IMAGE_LAYOUT_GENERAL, 
                     &outputClearValue, 1, &outputRange);
```

- [ ] **Step 3: Create ImGui texture descriptor for OutputTexture**

Add after the ImGui texture creation loop (after line ~178):

```cpp
// Create ImGui descriptor for output texture
if((ImGui::GetCurrentContext() != nullptr) && ImGui::GetIO().BackendPlatformUserData != nullptr)
{
  m_resources.outputTextureImID = reinterpret_cast<ImTextureID>(
      ImGui_ImplVulkan_AddTexture(m_createInfo.linearSampler, m_resources.outputTextureView, 
                                  VK_IMAGE_LAYOUT_GENERAL));
}
```

- [ ] **Step 4: Add OutputTexture cleanup in destroy() function**

Add before the ImGui texture removal loop (around line ~184):

```cpp
// Cleanup output texture
if(m_resources.outputTextureImID)
{
  using ImGuiTextureHandle = decltype(ImGui_ImplVulkan_AddTexture(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL));
  ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<ImGuiTextureHandle>(m_resources.outputTextureImID));
  m_resources.outputTextureImID = {};
}
if(m_resources.outputTextureImage != VK_NULL_HANDLE)
{
  vmaDestroyImage(m_allocator, m_resources.outputTextureImage, nullptr);
  m_resources.outputTextureImage = VK_NULL_HANDLE;
}
if(m_resources.outputTextureView != VK_NULL_HANDLE)
{
  vkDestroyImageView(m_device, m_resources.outputTextureView, nullptr);
  m_resources.outputTextureView = VK_NULL_HANDLE;
}
```

- [ ] **Step 5: Add getter implementations**

Add at the end of the file before the closing namespace:

```cpp
VkImageView SceneResources::getOutputTextureView() const
{
  return m_resources.outputTextureView;
}

ImTextureID SceneResources::getOutputTextureImID() const
{
  return m_resources.outputTextureImID;
}

VkImage SceneResources::getOutputTextureImage() const
{
  return m_resources.outputTextureImage;
}
```

- [ ] **Step 6: Build to verify compilation**

Run: `cmake --build build --config Release 2>&1 | head -30`
Expected: Build succeeds

---

### Task 3: Add kPassOutputHandle to Pass.h

**Files:**
- Modify: `render/Pass.h`

- [ ] **Step 1: Add OutputTexture handle constant**

Add after the existing handle constants (around line 79):

```cpp
inline constexpr TextureHandle kPassOutputHandle{0xF103u, 1u};
```

---

### Task 4: Add OutputTexture Interface to Renderer

**Files:**
- Modify: `render/Renderer.h`
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Add declaration in Renderer.h**

Add after `getSwapchainExtent()` (around line 176):

```cpp
VkImageView getOutputTextureView() const;
```

- [ ] **Step 2: Add implementation in Renderer.cpp**

Add after `getCurrentSwapchainImageView()` (around line 746):

```cpp
VkImageView Renderer::getOutputTextureView() const
{
  return m_swapchainDependent.sceneResources.getOutputTextureView();
}
```

---

### Task 5: Bind OutputTexture to PassExecutor

**Files:**
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Add OutputTexture binding in drawFrame()**

Add after the kPassGBufferDepthHandle binding (after line ~1017):

```cpp
m_passExecutor.bindTexture({
    .handle       = kPassOutputHandle,
    .nativeImage  = reinterpret_cast<uint64_t>(m_swapchainDependent.sceneResources.getOutputTextureImage()),
    .aspect       = rhi::TextureAspect::color,
    .initialState = rhi::ResourceState::general,
    .isSwapchain  = false,
});
```

- [ ] **Step 2: Build to verify compilation**

Run: `cmake --build build --config Release 2>&1 | head -30`
Expected: Build succeeds

---

### Task 6: Update LightPass to Render to OutputTexture

**Files:**
- Modify: `render/passes/LightPass.cpp`

- [ ] **Step 1: Update getDependencies() to use kPassOutputHandle**

Replace the `kPassSwapchainHandle` dependency with `kPassOutputHandle`:

```cpp
PassNode::HandleSlice<PassResourceDependency> LightPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 2> dependencies = {
        PassResourceDependency::texture(
            kPassGBufferColorHandle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassOutputHandle,  // Changed from kPassSwapchainHandle
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}
```

- [ ] **Step 2: Update execute() to use OutputTexture**

Replace the swapchain view handle with OutputTexture view. Change lines 41-47:

```cpp
// Get OutputTexture view and extent
rhi::TextureViewHandle outputViewHandle = rhi::TextureViewHandle::fromNative(
    m_renderer->getOutputTextureView());
const rhi::Extent2D extent = {
    SceneResources::kOutputTextureWidth,
    SceneResources::kOutputTextureHeight
};
```

- [ ] **Step 3: Update colorTarget to use OutputTexture**

Change line 58 to use the output view:

```cpp
rhi::RenderTargetDesc colorTarget = {
    .texture = {},  // Not used when view carries native pointer
    .view = outputViewHandle,  // Changed from swapchainViewHandle
    .state = rhi::ResourceState::general,
    .loadOp = rhi::LoadOp::clear,
    .storeOp = rhi::StoreOp::store,
    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
};
```

- [ ] **Step 4: Add SceneResources include for constants**

Add at the top after existing includes:

```cpp
#include "../SceneResources.h"
```

- [ ] **Step 5: Build to verify compilation**

Run: `cmake --build build --config Release 2>&1 | head -30`
Expected: Build succeeds

---

### Task 7: Update ForwardPass to Render to OutputTexture

**Files:**
- Modify: `render/passes/ForwardPass.cpp`

- [ ] **Step 1: Update getDependencies() to use kPassOutputHandle**

Replace the `kPassSwapchainHandle` dependency:

```cpp
PassNode::HandleSlice<PassResourceDependency> ForwardPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 2> dependencies = {
        PassResourceDependency::texture(
            kPassOutputHandle,  // Changed from kPassSwapchainHandle
            ResourceAccess::readWrite,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassGBufferDepthHandle,
            ResourceAccess::read,
            rhi::ShaderStage::fragment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}
```

- [ ] **Step 2: Update execute() to use OutputTexture**

Replace swapchain image view with OutputTexture view (lines 45-50):

```cpp
// Get OutputTexture view and extent
const VkImageView outputTextureView = m_renderer->getOutputTextureView();
const rhi::Extent2D renderExtent = {
    SceneResources::kOutputTextureWidth,
    SceneResources::kOutputTextureHeight
};
```

- [ ] **Step 3: Update render extent check**

Remove the swapchain extent code and simplify:

```cpp
if(outputTextureView == VK_NULL_HANDLE)
{
    context.cmd->endEvent();
    return;
}
```

- [ ] **Step 4: Update colorTarget to use OutputTexture**

Change lines 129-135:

```cpp
rhi::RenderTargetDesc colorTarget = {
    .texture = {},
    .view = rhi::TextureViewHandle::fromNative(outputTextureView),
    .state = rhi::ResourceState::general,
    .loadOp = rhi::LoadOp::load,  // Preserve LightPass output
    .storeOp = rhi::StoreOp::store,
};
```

- [ ] **Step 5: Build to verify compilation**

Run: `cmake --build build --config Release 2>&1 | head -30`
Expected: Build succeeds

---

### Task 8: Refactor PresentPass to Blit OutputTexture to Swapchain

**Files:**
- Modify: `render/passes/PresentPass.cpp`
- Modify: `render/passes/PresentPass.h`

- [ ] **Step 1: Add includes for blit operations**

Add to includes:

```cpp
#include "../SceneResources.h"
#include "../../rhi/vulkan/VulkanCommandList.h"
```

- [ ] **Step 2: Update getDependencies() to use kPassOutputHandle**

```cpp
PassNode::HandleSlice<PassResourceDependency> PresentPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 2> dependencies = {
        PassResourceDependency::texture(kPassOutputHandle, ResourceAccess::read, rhi::ShaderStage::fragment),
        PassResourceDependency::texture(kPassSwapchainHandle, ResourceAccess::write, rhi::ShaderStage::fragment),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}
```

- [ ] **Step 3: Implement blit logic in execute()**

Replace the entire execute() function:

```cpp
void PresentPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.params == nullptr)
        return;

    context.cmd->beginEvent("Present");

    VkCommandBuffer cmd = rhi::vulkan::getNativeCommandBuffer(*context.cmd);

    // Get swapchain image info
    const VkExtent2D swapchainExtent = m_renderer->getSwapchainExtent();
    const VkImage swapchainImage = reinterpret_cast<VkImage>(
        m_renderer->getSwapchainDependent().swapchain->getNativeImage(
            m_renderer->getSwapchainDependent().currentImageIndex));

    // Get OutputTexture info
    SceneResources& sceneResources = m_renderer->getSceneResources();
    const VkImage outputImage = sceneResources.getOutputTextureImage();

    // Transition swapchain to transfer dst
    {
        VkImageMemoryBarrier2 barrier{
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image               = swapchainImage,
            .subresourceRange    = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
        };
        VkDependencyInfo depInfo{
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }

    // Transition OutputTexture to transfer src
    {
        VkImageMemoryBarrier2 barrier{
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image               = outputImage,
            .subresourceRange    = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
        };
        VkDependencyInfo depInfo{
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }

    // Calculate letterbox/pillarbox region
    const float rtAspect = static_cast<float>(SceneResources::kOutputTextureWidth) 
                         / static_cast<float>(SceneResources::kOutputTextureHeight);
    const float swapAspect = static_cast<float>(swapchainExtent.width) 
                           / static_cast<float>(swapchainExtent.height);

    VkRect2D dstRect;
    if(swapAspect > rtAspect)
    {
        // Swapchain is wider - pillarbox (black bars on sides)
        dstRect.extent.width = static_cast<uint32_t>(swapchainExtent.height * rtAspect);
        dstRect.extent.height = swapchainExtent.height;
        dstRect.offset.x = (swapchainExtent.width - dstRect.extent.width) / 2;
        dstRect.offset.y = 0;
    }
    else
    {
        // Swapchain is taller - letterbox (black bars on top/bottom)
        dstRect.extent.width = swapchainExtent.width;
        dstRect.extent.height = static_cast<uint32_t>(swapchainExtent.width / rtAspect);
        dstRect.offset.x = 0;
        dstRect.offset.y = (swapchainExtent.height - dstRect.extent.height) / 2;
    }

    // Clear swapchain to black (for letterbox/pillarbox areas)
    {
        VkClearColorValue clearValue = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkImageSubresourceRange range{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
        vkCmdClearColorImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &range);
    }

    // Blit from OutputTexture to swapchain
    {
        VkImageBlit blitRegion{
            .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .srcOffsets     = {{0, 0, 0}, {static_cast<int32_t>(SceneResources::kOutputTextureWidth), 
                                            static_cast<int32_t>(SceneResources::kOutputTextureHeight), 1}},
            .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .dstOffsets     = {{static_cast<int32_t>(dstRect.offset.x), static_cast<int32_t>(dstRect.offset.y), 0},
                              {static_cast<int32_t>(dstRect.offset.x + dstRect.extent.width),
                               static_cast<int32_t>(dstRect.offset.y + dstRect.extent.height), 1}},
        };
        vkCmdBlitImage(cmd, outputImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blitRegion, VK_FILTER_LINEAR);
    }

    // Transition OutputTexture back to general
    {
        VkImageMemoryBarrier2 barrier{
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_BLIT_BIT,
            .srcAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask       = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .image               = outputImage,
            .subresourceRange    = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
        };
        VkDependencyInfo depInfo{
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &barrier,
        };
        vkCmdPipelineBarrier2(cmd, &depInfo);
    }

    // Begin dynamic rendering for ImGui
    m_renderer->beginPresentPass(*context.cmd);

    context.cmd->endEvent();
}
```

- [ ] **Step 4: Add getSwapchainDependent declaration if needed**

Check if Renderer has this method. If not, add to Renderer.h:
```cpp
const SwapchainDependentResources& getSwapchainDependent() const { return m_swapchainDependent; }
```

- [ ] **Step 5: Build to verify compilation**

Run: `cmake --build build --config Release 2>&1 | head -50`
Expected: Build succeeds

---

### Task 9: Update ImGui Viewport Texture Binding

**Files:**
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Update viewportAttachmentIndex to use OutputTexture**

Change line 528 from 0 to 3:

```cpp
m_materials.viewportTextureHandle = m_materials.texturePool.emplace(MaterialResources::TextureRecord{
    .hot =
        {
            .runtimeKind             = MaterialResources::TextureRuntimeKind::viewportAttachment,
            .viewportAttachmentIndex = SceneResources::kOutputTextureIndex,  // Changed from 0 to 3
        },
});
```

- [ ] **Step 2: Update getViewportTextureID to handle OutputTexture**

The existing code uses `getImTextureID(viewportAttachmentIndex)`. We need to add special handling for the OutputTexture index. Modify the function:

```cpp
ImTextureID Renderer::getViewportTextureID(TextureHandle handle) const
{
  const MaterialResources::TextureHotData* textureHot = tryGetTextureHot(handle);
  if(textureHot == nullptr)
  {
    LOGW("Renderer::getViewportTextureID rejected stale/invalid texture handle (index=%u generation=%u)", handle.index,
         handle.generation);
    return ImTextureID{};
  }

  if(textureHot->runtimeKind == MaterialResources::TextureRuntimeKind::viewportAttachment)
  {
    // Check if it's the OutputTexture
    if(textureHot->viewportAttachmentIndex == SceneResources::kOutputTextureIndex)
    {
      return m_swapchainDependent.sceneResources.getOutputTextureImID();
    }
    return m_swapchainDependent.sceneResources.getImTextureID(textureHot->viewportAttachmentIndex);
  }

  return ImTextureID{};
}
```

- [ ] **Step 3: Build to verify compilation**

Run: `cmake --build build --config Release 2>&1 | head -30`
Expected: Build succeeds

---

### Task 10: Build and Test

**Files:**
- N/A

- [ ] **Step 1: Full build**

Run: `cmake --build build --config Release`
Expected: Build succeeds without errors

- [ ] **Step 2: Run application**

Run: `./build/Release/Demo.exe`
Expected: Application starts without validation errors

- [ ] **Step 3: Verify ImGui viewport shows PBR result**

Check that the ImGui viewport displays the PBR-lit scene instead of the GBuffer base color.

- [ ] **Step 4: Verify letterbox/pillarbox**

Resize window to non-16:9 aspect ratio and verify black bars appear correctly.

---

### Task 11: Commit Changes

- [ ] **Step 1: Commit all changes**

```bash
git add render/SceneResources.h render/SceneResources.cpp render/Pass.h render/Renderer.h render/Renderer.cpp render/passes/LightPass.cpp render/passes/ForwardPass.cpp render/passes/PresentPass.cpp
git commit -m "refactor(render): add offscreen rendering pipeline with OutputTexture

- Add OutputTexture (1920x1080 sRGB) to SceneResources
- LightPass and ForwardPass now render to OutputTexture
- PresentPass blits OutputTexture to swapchain with letterbox support
- ImGui viewport now displays final PBR result instead of GBuffer[0]"
```