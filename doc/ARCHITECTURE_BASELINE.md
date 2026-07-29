# Vulkan Minimal Latest - Architecture Baseline

> **Historical Snapshot**: Captures the architecture of `src/minimal_latest.cpp` as it existed before the renderer/RHI split.
> **Generated From**: `src/minimal_latest.cpp` (3669 lines), `README.md`
> **Date**: 2026-03-31
> **Current Renderer Delta**: The current renderer creates renderer-owned 1x1 white/normal RGBA fallback textures. It does not load `image1.jpg`/`image2.jpg` from the process working directory and has no fixed two-image member array.

## 1. Overview

The historical baseline implementation was a single-file Vulkan sample demonstrating modern Vulkan 1.4 patterns. The entire application was contained within the `MinimalLatest` class (~2185-3669 lines), which owned all Vulkan resources, window management, and rendering logic.

## 2. Main Classes

### 2.1 MinimalLatest (Main Application Class)
**Location**: `src/minimal_latest.cpp:2185-3669`

The monolithic application class containing:
- GLFW window management
- Vulkan context initialization
- Resource allocation and management
- Frame rendering loop
- UI (ImGui) integration

### 2.2 Context (Vulkan Instance/Device Management)
**Location**: `src/minimal_latest.cpp:715-1081`

Manages:
- Vulkan instance creation
- Physical device selection
- Logical device creation
- Queue management
- Extension handling

### 2.3 Swapchain (Presentation Layer)
**Location**: `src/minimal_latest.cpp:1096-1454`

Manages:
- Swapchain creation/recreation
- Swapchain images and views
- Presentation semaphores (binary)
- Frame resource indexing

### 2.4 ResourceAllocator (VMA Wrapper)
**Location**: `src/minimal_latest.cpp:1554-1777`

Wraps Vulkan Memory Allocator for:
- Buffer allocation
- Image allocation
- Staging buffer management
- Memory defragmentation

### 2.5 SamplerPool (Sampler Caching)
**Location**: `src/minimal_latest.cpp:1783-1878`

Caches samplers by hash to avoid duplicates.

### 2.6 Gbuffer (Render Target)
**Location**: `src/minimal_latest.cpp:1914-2147`

Manages:
- Color attachment (for viewport rendering)
- Depth attachment
- Associated image views and descriptors

### 2.7 FramePacer (Frame Rate Control)
**Location**: `src/minimal_latest.cpp:1470-1513`

Limits frame rate to monitor refresh rate when vSync is enabled.

## 3. Initialization Sequence

**Entry Point**: `MinimalLatest::init()` ~2326

```
1. volkInitialize()                          // Load Vulkan functions
2. glfwCreateWindow()                        // Create GLFW window

3. Context Creation (~2343):
   - Create Vulkan instance
   - Select physical device
   - Create logical device
   - Get graphics queue

4. ResourceAllocator Init (~2346):
   - Initialize VMA

5. SamplerPool Init (~2354)

6. Surface Creation (~2357):
   - glfwCreateWindowSurface()

7. Transient Command Pool (~2362)

8. Swapchain Setup (~2365):
   - Swapchain.init()
   - swapchain.initResources() // Creates swapchain images

9. Frame Submission Setup (~2369):
   - createFrameSubmission()
   - Creates timeline semaphore
   - Creates per-frame command pools/buffers

10. Descriptor Pool (~2372)

11. ImGui Init (~2375)

12. GBuffer Creation (~2383-2398):
    - Single-time command buffer
    - GBuffer.init()

13. Graphics Descriptor Set (~2401)

14. Graphics Pipeline (~2404)

15. Compute Pipeline (~2407)

16. GPU Buffers Upload (~2410-2432):
    - Vertex buffer (SSBO)
    - Points buffer
    - Historical baseline: load image1.jpg and image2.jpg

17. Scene Info Buffer (~2436)

18. Update Graphics Descriptor Set (~2442)
```

## 4. Per-Frame Flow

**Entry Point**: `MinimalLatest::run()` ~2211

### 4.1 Main Loop Structure

```cpp
while(!glfwWindowShouldClose(m_window))
{
    // 1. Frame pacing
    m_framePacer.paceFrame(...);
    
    // 2. Poll events
    glfwPollEvents();
    
    // 3. Skip if minimized
    if(glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE)
        continue;
    
    // 4. ImGui new frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    
    // 5. Docking setup
    ImGui::DockSpaceOverViewport(...);
    
    // 6. Menu bar
    // 7. Viewport sizing
    // 8. Check viewport resize
    
    // 9. Frame rendering (if preparation succeeds)
    if(prepareFrameResources())
    {
        VkCommandBuffer cmd = beginCommandRecording();
        drawFrame(cmd);
        endFrame(cmd);
    }
    
    // 10. Platform windows update
}
```

### 4.2 Frame Preparation

**Function**: `prepareFrameResources()` ~3587

```
1. Check if swapchain needs rebuild
   - If yes: wait idle, reinitResources()

2. Wait for previous frame completion
   - vkWaitSemaphores(m_frameTimelineSemaphore, frame.lastSignalValue)

3. Acquire next swapchain image
   - swapchain.acquireNextImage()
   - Returns m_frameImageIndex

4. Reset command pool for current frame
   - vkResetCommandPool(m_frameData[ring].cmdPool)

5. Begin command buffer recording
   - vkBeginCommandBuffer()
```

### 4.3 Draw Frame Sequence

**Function**: `drawFrame(cmd)` ~2596

```
1. ImGui Viewport Display
   - ImGui::Begin("Viewport")
   - ImGui::Image(m_gBuffer.getImTextureID(0), ...)  // Display GBuffer
   - ImGui::End()

2. Settings Window (ImGui)

3. Record Compute Commands ~2797
   - Dispatch compute shader for vertex animation
   - Barrier: COMPUTE_SHADER -> VERTEX_INPUT

4. Record Graphic Commands ~2859
   - Update scene buffer (UBO)
   - Begin dynamic rendering to GBuffer
   - Push descriptor set (scene info)
   - Draw triangle (pipeline without texture)
   - Draw textured triangle (pipeline with texture)
   - End rendering

5. Begin Dynamic Rendering to Swapchain ~2645
   - Transition swapchain to GENERAL
   - Begin rendering

6. ImGui Render ~2648
   - ImGui_ImplVulkan_RenderDrawData()

7. End Dynamic Rendering ~2650
   - End rendering
   - Transition swapchain to PRESENT_SRC_KHR
```

### 4.4 Frame Submission

**Function**: `endFrame(cmd)` ~2660

```
1. End command buffer
   - vkEndCommandBuffer()

2. Calculate signal value
   - signalValue = m_frameCounter++
   - frame.lastSignalValue = signalValue

3. Submit to queue
   - vkQueueSubmit2()
   - Wait: imageAvailableSemaphore (binary)
   - Signal: renderFinishedSemaphore (binary)
   - Signal: m_frameTimelineSemaphore (timeline, signalValue)

4. Present
   - swapchain.presentFrame()
   - Wait: renderFinishedSemaphore

5. Advance frame ring
   - m_frameRingCurrent = (m_frameRingCurrent + 1) % numFrames
```

## 5. Resource Ownership

### 5.1 Historical Resource Members (MinimalLatest Snapshot)

**Location**: `src/minimal_latest.cpp:3534-3579`

The historical snapshot kept `image1.jpg` and `image2.jpg` in a fixed two-element `utils::ImageResource` array. The current renderer has no equivalent fixed member; current fallback ownership is described in the header note above.

```cpp
// Window & Surface
GLFWwindow* m_window;                          // GLFW window
VkSurfaceKHR m_surface;                        // Window surface
VkExtent2D m_windowSize;                       // Window dimensions
VkExtent2D m_viewportSize;                     // Viewport dimensions

// Core Vulkan
utils::Context m_context;                      // Instance/device/queues
utils::ResourceAllocator m_allocator;          // VMA allocator
utils::Swapchain m_swapchain;                  // Presentation
utils::SamplerPool m_samplerPool;              // Sampler cache
utils::Gbuffer m_gBuffer;                      // Render target

// Buffers
utils::Buffer m_vertexBuffer;                  // SSBO - animated vertices
utils::Buffer m_pointsBuffer;                  // SSBO - points data
utils::Buffer m_sceneInfoBuffer;               // UBO - scene uniforms

// Pipelines
VkPipelineLayout m_graphicPipelineLayout;      // Graphics layout
VkPipelineLayout m_computePipelineLayout;      // Compute layout
VkPipeline m_computePipeline;                  // Compute (vertex animation)
VkPipeline m_graphicsPipelineWithTexture;      // Graphics (textured)
VkPipeline m_graphicsPipelineWithoutTexture;   // Graphics (colored)

// Command Pools
VkCommandPool m_transientCmdPool;              // Short-lived operations

// Descriptors
VkDescriptorPool m_descriptorPool;             // Main descriptor pool
VkDescriptorPool m_uiDescriptorPool;           // ImGui descriptor pool
VkDescriptorSetLayout m_textureDescriptorSetLayout;
VkDescriptorSetLayout m_graphicDescriptorSetLayout;
VkDescriptorSet m_textureDescriptorSet;        // Texture descriptor

// Per-Frame Data
std::vector<FrameData> m_frameData;            // Per-frame resources
VkSemaphore m_frameTimelineSemaphore;          // GPU-CPU sync
uint64_t m_frameCounter;                       // Monotonic counter
uint32_t m_frameRingCurrent;                   // Current frame index

// Frame Pacing & State
FramePacer m_framePacer;                       // Frame rate limiter
bool m_vSync;                                  // vSync enabled
int m_imageID;                                 // Current texture selection
static const uint32_t m_maxTextures = 2;       // Max textures
VkClearColorValue m_clearColor;                // Clear color
```

### 5.2 Per-Frame Resources (FrameData)

**Location**: `src/minimal_latest.cpp:3564-3573`

```cpp
struct FrameData
{
    VkCommandPool cmdPool;           // Command pool for this frame
    VkCommandBuffer cmdBuffer;       // Command buffer for this frame
    uint64_t lastSignalValue;        // Last timeline value signaled
};
```

One per frame-in-flight (typically 2-3 frames).

## 6. Resize/Rebuild Paths

### 6.1 Viewport Resize (GBuffer Only)

**Trigger**: Viewport size change in ImGui (~2267)
**Function**: `onViewportSizeChange()` ~2738

```
1. Check if viewport size changed
2. vkQueueWaitIdle()
3. Update GBuffer size
4. GBuffer.update(cmd, newSize)  // Recreate color/depth images
```

**Scope**: Only GBuffer attachments recreated.

### 6.2 Swapchain Rebuild (Full Window)

**Trigger**: 
- vSync toggle (~2249)
- VK_ERROR_OUT_OF_DATE_KHR

**Function**: `prepareFrameResources()` ~3590

```
1. Check m_swapchain.needRebuilding()
2. vkQueueWaitIdle()
3. m_swapchain.reinitResources()
   - Destroy old swapchain
   - Destroy semaphores
   - Create new swapchain
   - Create new images/views
   - Create new semaphores
```

**Scope**: Swapchain images, views, semaphores recreated.

## 7. Component Relationships

### 7.1 Frame Data Flow

```
[Compute Shader] -> m_vertexBuffer (SSBO)
                        |
                        v
[Graphics Pipeline] -> GBuffer (color + depth)
                        |
                        v
[ImGui::Image] -> Viewport display (in UI)
                        |
                        v
[Dynamic Rendering] -> Swapchain image
                        |
                        v
[Present]
```

### 7.2 Synchronization Primitives

| Primitive | Type | Purpose |
|-----------|------|---------|
| imageAvailableSemaphore | Binary | Swapchain acquire -> GPU ready |
| renderFinishedSemaphore | Binary | GPU render complete -> Present |
| m_frameTimelineSemaphore | Timeline | GPU-CPU frame pacing |
| m_frameCounter | uint64_t | Monotonic signal values (1, 2, 3...) |
| m_frameData[].lastSignalValue | uint64_t | Per-frame completion tracking |

### 7.3 Timeline Semaphore Flow

```
Frame 0: wait(0) -> signal(1) -> lastSignalValue[0] = 1
Frame 1: wait(0) -> signal(2) -> lastSignalValue[1] = 2
Frame 2: wait(0) -> signal(3) -> lastSignalValue[2] = 3
Frame 3: wait(1) -> signal(4) -> lastSignalValue[0] = 4  (reuses slot 0)
```

## 8. Pipeline Configuration

### 8.1 Graphics Pipelines

**With Texture** (~2992):
- Vertex shader with texture sampling
- Fragment shader with texture lookup
- Used for textured triangle

**Without Texture** (~2992):
- Vertex shader with vertex colors
- Fragment shader with color output
- Used for colored/animated triangle

### 8.2 Compute Pipeline

**Vertex Animation** (~3480):
- Updates vertex positions using compute shader
- Reads/writes to m_vertexBuffer (SSBO)
- Synchronized with graphics via barrier

## 9. Descriptor Sets

### 9.1 Set 0: Scene Globals
- Binding 0: SceneInfo UBO (transform, time)
- Binding 1: Vertex SSBO
- Binding 2: Points SSBO

### 9.2 Set 1: Material (Texture)
- Binding 0: Texture image
- Binding 1: Sampler

**Current Implementation**: Uses push descriptors (vkCmdPushDescriptorSet2KHR)

## 10. Key Observations

### 10.1 Monolithic Design
- All resources owned by `MinimalLatest` class
- No clear separation between App and Renderer layers
- Window management mixed with Vulkan code

### 10.2 Resource Lifetimes
- **Device Lifetime**: Context, Allocator, SamplerPool
- **Swapchain Lifetime**: Swapchain images/views, GBuffer
- **Frame Lifetime**: Per-frame command pools/buffers, staging buffers
- **Persistent**: Pipelines, descriptor layouts, buffers, images

### 10.3 Frame Loop Coupling
- UI rendering mixed with scene rendering
- Command recording happens in drawFrame()
- No explicit "pass" abstraction

### 10.4 Resize Handling
- Two separate resize paths (viewport vs swapchain)
- No unified rebuild boundary
- Some resources recreated unnecessarily on viewport resize

---

## Appendix: Line Number Reference

| Symbol | Line Range | Description |
|--------|------------|-------------|
| MinimalLatest class | 2185-3669 | Main application class |
| run() | 2211-2300 | Main loop |
| init() | 2326-2443 | Initialization |
| destroy() | 2448-2491 | Cleanup |
| drawFrame() | 2596 | Per-frame render |
| recordComputeCommands() | 2797 | Compute dispatch |
| recordGraphicCommands() | 2859 | Graphics commands |
| createGraphicsPipeline() | 2992 | Pipeline creation |
| initImGui() | 3211 | ImGui setup |
| createDescriptorPool() | 3252 | Descriptor pool |
| prepareFrameResources() | 3587 | Frame preparation |

---

**End of Baseline Document**
