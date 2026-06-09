# RHI 后端映射表（BCK-03 验收文档）

**文档版本：** Phase 6 Plan 04（06-04）
**状态：** BCK-03 语义证据（D-10/D-11）
**关联提交：** 06-01 ~ 06-03b（编译级证据见下方"编译门"节）

---

## 概述

本文档是 Phase 6 **BCK-03** 验收的语义证据，证明 public RHI contract（`rhi/*.h` 公共头）
未被 Vulkan 专有语义锁死，可被 Vulkan / D3D12 / Metal 三后端按能力方式映射。

**D-10 双重证据结构：**

- **(a) 编译级硬证据：** D3D12/Metal stub 实现了 public contract 的全部虚函数签名，cmake
  构建零错误（Phase 06-01~06-03b 已验证；详见本文最后"编译门"节）。
- **(b) 语义文档证据（本文档）：** 按 D-11 规定的六大 seam，为每个 public RHI 概念提供
  Vulkan / D3D12 / Metal 三后端对应或明确标注 unsupported。

**说明：** D3D12 和 Metal 后端目前为 `RHI_UNIMPLEMENTED`（LOGE + std::abort）stub，
v2 阶段（BCK-04）将实现真实帧渲染。下表标注"stub / unsupported (v2)"的行表示：
stub 已编译通过（接口契约已满足），但运行时调用会 abort；右侧"预期映射"列说明 v2
实现该接口时应对应的 backend 概念，证明契约的语义可映射性。

---

## Resource（资源）

覆盖纹理（Texture）、缓冲区（Buffer）、视图（TextureView）、采样器（Sampler）
的创建、注册与销毁路径。

### Texture（RHIDevice::createTexture / TextureHandle）

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `createTexture(TextureDesc)` | `vkCreateImage` + VMA `vmaCreateImage` | `ID3D12Device::CreateCommittedResource` / `CreatePlacedResource` | `MTLDevice.newTexture(descriptor:)` | `TextureDesc` 中 `TextureFormat`、`TextureUsageFlags`、`Extent3D` 均为可移植类型，后端负责 lowering |
| `destroyTexture(TextureHandle)` | `vmaDestroyImage` | 释放 `ID3D12Resource` | 释放 `id<MTLTexture>` | |
| `registerExternalTexture(uint64_t)` | 把外部 `VkImage` token 注册进句柄表（不接管所有权） | 注册外部 `ID3D12Resource*` | 注册外部 `id<MTLTexture>` | 主要用途：swapchain 纹理 |
| `destroyImage(TextureHandle)` | 同 destroyTexture（legacy alias） | 同上 | 同上 | 过渡期别名 |
| `TextureFormat` 枚举 | `VkFormat`（`RHIFormatBridge.h` lowering，backend-internal） | `DXGI_FORMAT`（backend-internal lowering） | `MTLPixelFormat`（backend-internal lowering） | Phase 2 已完成 `TextureFormat` 可移植化（D-01/D-02） |
| `TextureUsageFlags` 枚举 | `VkImageUsageFlagBits`（backend-internal） | D3D12 resource flags / `D3D12_RESOURCE_STATES` 初始化 | `MTLTextureUsage`（backend-internal） | |
| `MemoryUsage` 枚举 | VMA `VmaMemoryUsage`（backend-internal） | D3D12 heap type（UPLOAD/READBACK/DEFAULT） | Metal storage mode（Shared/Private/Managed） | |

### TextureView（RHIDevice::createTextureView / TextureViewHandle）

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `createTextureView(TextureViewCreateDesc)` | `vkCreateImageView`，结果存入 VulkanDevice 内部 view registry | `ID3D12Device::CreateShaderResourceView` / `CreateRenderTargetView`（descriptor heap entry） | `id<MTLTexture>.newTextureView(pixelFormat:...)` | `TextureViewCreateDesc` 含 `ImageViewType`、`TextureAspect`、mip/layer range；均可移植，backend 负责 lowering |
| `registerExternalTextureView(uint64_t)` | 注册外部 VkImageView token（不接管所有权） | 注册外部 D3D12 descriptor handle | 注册外部 MTLTexture view | 主要用途：swapchain image view |
| `destroyTextureView(TextureViewHandle)` | `vkDestroyImageView` | 释放 D3D12 RTV/SRV descriptor | 释放 MTLTexture view | |
| native 句柄解析 | `VulkanDeviceInterop::resolveTextureView()` → `VkImageView`（backend-internal） | backend-internal（stub，v2 实现） | backend-internal（stub，v2 实现） | 公共层不持有 VkImageView，D-07 已移除 escape getter |

### Buffer（RHIDevice::createBuffer / BufferHandle）

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `createBuffer(BufferDesc)` | `vkCreateBuffer` + VMA alloc | `ID3D12Device::CreateCommittedResource`（buffer） | `MTLDevice.newBuffer(length:options:)` | stub / unsupported (v2) for D3D12/Metal |
| `destroyBuffer(BufferHandle)` | VMA destroy | 释放 `ID3D12Resource` | 释放 `id<MTLBuffer>` | stub / unsupported (v2) |
| `getBufferGpuAddress(BufferHandle)` | `VkBufferDeviceAddressInfo` → `GpuPtr` | `ID3D12Resource::GetGPUVirtualAddress()` | Metal buffer GPU address（`MTLBuffer.gpuAddress`，macOS 13+） | stub / unsupported (v2) |
| `mapBuffer` / `unmapBuffer` | `vmaMapMemory` / `vmaUnmapMemory` | `ID3D12Resource::Map` / `Unmap` | `MTLBuffer.contents()` | stub / unsupported (v2) |
| `registerExternalBuffer(uint64_t)` | 注册外部 VkBuffer token | 注册外部 D3D12 buffer | 注册外部 MTLBuffer | stub / unsupported (v2) |
| `BufferUsageFlags` | `VkBufferUsageFlagBits`（backend-internal） | D3D12 resource state flags | `MTLResourceUsage`（backend-internal） | |
| `GpuPtr` | `VkDeviceAddress`（uint64_t 封装） | `D3D12_GPU_VIRTUAL_ADDRESS` | Metal buffer GPU address | 类型封装屏蔽 backend 差异 |

### Sampler（RHIDevice::createSampler / SamplerHandle）

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `createSampler(SamplerDesc)` | `vkCreateSampler` | D3D12 sampler descriptor heap entry | `MTLDevice.newSamplerState(descriptor:)` | stub / unsupported (v2) |
| `destroySampler(SamplerHandle)` | `vkDestroySampler` | 释放 descriptor | 释放 MTLSamplerState | stub / unsupported (v2) |
| `SamplerDesc` 字段 | `VkSamplerCreateInfo`（lowering） | `D3D12_SAMPLER_DESC`（lowering） | `MTLSamplerDescriptor`（lowering） | Filter/AddressMode/MipmapMode 均为可移植枚举 |

---

## Pipeline（管线）

覆盖图形管线、计算管线、ShaderStage、绑定 schema 的三后端映射。

### GraphicsPipeline（RHIDevice::createGraphicsPipeline / PipelineHandle）

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `createGraphicsPipeline(GraphicsPipelineDesc)` | `vkCreateGraphicsPipelines`（含 `VkPipelineLayout` backend-internal 构建） | `ID3D12Device::CreateGraphicsPipelineState`（PSO） | `MTLDevice.newRenderPipelineState(descriptor:)` | stub / unsupported (v2) for D3D12/Metal；`VkPipelineLayout` 不暴露给上层 |
| `createComputePipeline(ComputePipelineDesc)` | `vkCreateComputePipelines` | `ID3D12Device::CreateComputePipelineState` | `MTLDevice.newComputePipelineState(descriptor:)` | stub / unsupported (v2) |
| `destroyPipeline(PipelineHandle)` | `vkDestroyPipeline` + `vkDestroyPipelineLayout` | 释放 D3D12 PSO | 释放 MTLRenderPipelineState | |
| `PipelineBindingSchemaDesc`（argumentSlots / rootBindings） | 后端 lowering 为 `VkDescriptorSetLayout` + `VkPipelineLayout`（backend-internal） | 后端 lowering 为 root signature | 后端 lowering 为 argument buffer layout | 公共契约只暴露逻辑槽（logical slot），不暴露 VkDescriptorSet 编号 |
| `PipelinePushConstantRange` | `VkPushConstantRange`（VkPipelineLayout 内部） | D3D12 root constants（root signature）内部 | Metal root buffer / per-stage constants 内部 | 上层只传逻辑 slot + size，不感知 backend 实现方式 |
| `PipelineShaderStageDesc`（SPIR-V 字节码） | `vkCreateShaderModule`（backend-internal，RDEV-02 已完成） | backend transpile SPIR-V→DXIL（v2）or 直接传 DXIL | backend transpile SPIR-V→MSL（v2）or 传 metallib | VkShaderModule 不暴露给上层（D-07 结论）；详见下方 Shader Module 节 |
| `GraphicsPipelineDesc` 中 RasterState/DepthState/BlendState | `VkPipelineRasterizationStateCreateInfo` 等（lowering） | D3D12 PSO desc 对应字段（lowering） | MTLRenderPipelineDescriptor 对应字段（lowering） | 均为可移植 POD，无 Vk* 类型泄漏 |

### PipelineCompiler（异步编译，能力门控）

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `createPipelineCompiler(PipelineCompileOptions)` | Vulkan pipeline cache + 异步编译包装 | D3D12 PSO cache + 后台线程编译 | MTLBinaryArchive / 后台 MTLRenderPipelineState 构建 | stub / unsupported (v2)；能力门 `CapabilityTier::PipelineCompiler` |
| `destroyPipelineCompiler(PipelineCompilerHandle)` | 同上销毁路径 | 同上 | 同上 | |

---

## Barrier / Layout 转换

覆盖 `ResourceState` 枚举、StageBarrier（主路径）、ResourceBarrier（显式转换路径）。

### 核心同步概念对照

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `StageFlags`（producer/consumer 阶段） | `VkPipelineStageFlagBits2`（backend-internal lowering） | D3D12 enhanced barrier access scopes（Win11 22H2+）或手动 transition barriers | Metal 隐式同步（automatic hazard tracking）；显式路径用 `MTLFence` / `MTLEvent` | 上层只传逻辑阶段，不感知 VkPipelineStageFlagBits 值 |
| `HazardFlags`（hazard class） | `VkAccessFlagBits2`（backend-internal） | D3D12 subresource state flags | Metal resource hazard mode | |
| `CommandBuffer::barrier(producer, consumer, hazards)` | `vkCmdPipelineBarrier2`（backend-internal） | `ID3D12GraphicsCommandList::ResourceBarrier`（enhanced）或 global barriers | `id<MTLCommandEncoder>` fence/event wait | stub / unsupported (v2) for D3D12/Metal |

### ResourceState 枚举映射（显式资源转换路径）

`ResourceState` 是上层语义状态（portably named）；`VkImageLayout` 是 Vulkan 内部 lowering 细节。
上层代码不感知 `VkImageLayout` 值，VulkanDevice 在 `resourceBarrier()` 内部负责从 `ResourceState` 到
`VkImageLayout` 的转换。

| ResourceState 枚举值 | Vulkan internal（VkImageLayout） | D3D12 internal（D3D12_RESOURCE_STATES） | Metal internal | 备注 |
|--------------------|--------------------------------|---------------------------------------|----------------|------|
| `Undefined` | `VK_IMAGE_LAYOUT_UNDEFINED` | `D3D12_RESOURCE_STATE_COMMON`（或 discard） | 无初始化（不保证） | |
| `General` | `VK_IMAGE_LAYOUT_GENERAL` | `D3D12_RESOURCE_STATE_COMMON` | 通用（无特定 layout） | |
| `ColorAttachment` | `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` | `D3D12_RESOURCE_STATE_RENDER_TARGET` | `MTLLoadActionLoad` / render target binding | |
| `DepthStencilAttachment` | `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL` | `D3D12_RESOURCE_STATE_DEPTH_WRITE` | depth attachment（MTLRenderPassDepthAttachmentDescriptor） | |
| `DepthStencilReadOnly` | `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL` | `D3D12_RESOURCE_STATE_DEPTH_READ` | depth read-only binding | |
| `ShaderRead` | `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` | `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` | `MTLTextureUsageShaderRead`（Metal 隐式） | |
| `ShaderWrite` | `VK_IMAGE_LAYOUT_GENERAL`（UAV） | `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` | `MTLTextureUsageShaderWrite`（Metal 隐式） | |
| `TransferSrc` | `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` | `D3D12_RESOURCE_STATE_COPY_SOURCE` | Metal copy source（blit encoder） | |
| `TransferDst` | `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` | `D3D12_RESOURCE_STATE_COPY_DEST` | Metal copy destination（blit encoder） | |
| `Present` | `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` | `D3D12_RESOURCE_STATE_PRESENT` | `MTLDrawable.present()` 前的 drawable 状态 | |

**关键点：** `ResourceState` 枚举值**不等于**对应的 `VkImageLayout` 整数值；上层代码不应假设
两者等价。`VkImageLayout` 的 lowering 逻辑完全封装在 VulkanDevice backend 内部。

### Buffer Barrier

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `BufferBarrier`（before/after ResourceState） | `VkBufferMemoryBarrier2`（backend-internal）| D3D12 `D3D12_RESOURCE_TRANSITION_BARRIER`（buffer） | Metal fence / buffer hazard mode | stub / unsupported (v2) for D3D12/Metal |

---

## Argument Table / Descriptor

覆盖 `ArgumentLayout`、`ArgumentTable`、`ArgumentWrite` 到三后端 descriptor 系统的映射。
**关键：** `VkDescriptorSet` / `VkDescriptorSetLayout` 完全封装在 VulkanDevice 内部；上层只传 RHI handle。

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `createArgumentLayout(ArgumentLayoutDesc)` | `vkCreateDescriptorSetLayout`（backend-internal） | D3D12 root signature slot desc（backend-internal）或 descriptor table range | `MTLArgumentDescriptor` array（MTLArgumentEncoder） | stub / unsupported (v2) for D3D12/Metal |
| `destroyArgumentLayout(ArgumentLayoutHandle)` | `vkDestroyDescriptorSetLayout` | 释放 root signature 引用 | 释放 MTLArgumentDescriptor | stub / unsupported (v2) |
| `createArgumentTable(ArgumentLayoutHandle)` | `vkAllocateDescriptorSets`（从 VulkanDevice 内部 descriptor pool 分配） | D3D12 descriptor heap range 分配 | `MTLDevice.newArgumentEncoder(arguments:)` 创建的 encoder + buffer | stub / unsupported (v2) |
| `destroyArgumentTable(ArgumentTableHandle)` | `vkFreeDescriptorSets` | 释放 D3D12 descriptor heap range | 释放 MTLBuffer（argument buffer） | stub / unsupported (v2) |
| `updateArgumentTable(handle, writes[])` | `vkUpdateDescriptorSets`（`VkWriteDescriptorSet`，backend-internal） | `ID3D12Device::CopyDescriptors` 或 `CreateShaderResourceView` 到堆 | `MTLArgumentEncoder.setTexture/setBuffer/setSamplerState` | stub / unsupported (v2)；`ArgumentWrite` 含 RHI handle，不含 VkImageView |
| `ArgumentType` 枚举 | `VkDescriptorType`（backend-internal lowering） | D3D12 descriptor type（SRV/UAV/CBV/Sampler） | MTL argument type（texture/buffer/sampler） | |
| `RenderEncoder::setArgumentTable(stages, slot, table)` | `vkCmdBindDescriptorSets`（backend slot → set index） | `ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable` | `setVertexBuffer/setFragmentBuffer`（argument buffer GPU address） | 上层只传逻辑 slot，不感知 VkDescriptorSet 编号 |
| `DescriptorHeap`（`allocateDescriptorHeap`，能力门控） | Vulkan bindless pool（backend-internal，能力 `DescriptorHeap`） | D3D12 shader-visible descriptor heap（`D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE`） | Metal argument buffer tier 2（`supportsArgumentBuffers`） | stub / unsupported (v2) |

---

## Shader Module

覆盖 `ShaderLibraryDesc` / `ShaderLibraryHandle` 到三后端 shader 编译/加载的映射。
**关键：** `VkShaderModule` 是 Vulkan 后端内部实现细节，上层不持有 VkShaderModule 句柄。

| RHI 概念 | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------|--------------|------------------|--------------------|------|
| `PipelineShaderStageDesc`（spirvCode + spirvSize） | `vkCreateShaderModule`（backend-internal，RDEV-02 完成）；完成后立即销毁或延迟到 pipeline 创建后销毁 | v2 透传 SPIR-V 给 DXC 转译为 DXIL，或直接接受 DXIL blob | v2 透传 SPIR-V 给 spirv-cross 转译为 MSL，或直接接受 .metallib | `ShaderIRFormat::spirv` 为当前路径；D3D12 stub 接受但 abort；Metal stub 同 |
| `createShaderLibrary(ShaderLibraryDesc)` | 若 format=spirv：`vkCreateShaderModule` 包装；若 format=dxil/metalLibrary：abort（backend mismatch） | format=dxil：`ID3D12Device::CreateLibrary`（D3D12 shader reflection）；format=spirv：transpile | format=metalLibrary：`MTLDevice.makeLibrary(data:)`；format=spirv：transpile | stub / unsupported (v2)；ShaderIRFormat 枚举已可移植（spirv/dxil/metalLibrary） |
| `destroyShaderLibrary(ShaderLibraryHandle)` | 内部销毁 VkShaderModule | 销毁 D3D12 shader blob | 释放 MTLLibrary | stub / unsupported (v2) |
| `ShaderIRFormat` 枚举 | `spirv`（v1 path）；其余 abort | `dxil`（v2 path）；spirv 需 transpile | `metalLibrary`（v2 path）；spirv 需 transpile | 上层通过 format 字段声明 IR 类型，不感知 backend 细节 |
| `PipelineCompilerHandle`（能力门控） | Vulkan pipeline cache + async specialization | D3D12 PSO cache + DXC async | Metal binary archive + async MTLRenderPipelineState | stub / unsupported (v2)；见 Pipeline 节 |

---

## Capability（能力查询）

覆盖 `supports(CapabilityTier)` / `CapabilityReport` / `CapabilityRequirements` 到三后端能力查询机制的映射。

### CapabilityTier 枚举映射

| CapabilityTier | Vulkan（实现） | D3D12（预期 / v2） | Metal（预期 / v2） | 备注 |
|---------------|--------------|------------------|--------------------|------|
| `Core`（coreGraphics + coreCompute + coreBindless） | Vulkan 1.2+ 设备 + descriptor indexing extension 检查 | `D3D_FEATURE_LEVEL_12_0+` + Shader Model 6.5+（bindless descriptor heap） | `MTLGPUFamilyApple1+` + argument buffer tier 1 | v1 必须 floor；D3D12/Metal stub: `queryCapabilities()` 返回零值 capabilities（stub，v2 前始终 false） |
| `ExtensionAsyncCompute` | `VkQueue` compute family + `VkCommandPool` 独立 compute queue | D3D12 `D3D12_COMMAND_LIST_TYPE_COMPUTE` queue | `MTLCommandQueue`（Metal 所有队列均支持 compute，隐式） | stub / unsupported (v2) |
| `ExtensionMeshShader` | `VK_EXT_mesh_shader` + `VkPhysicalDeviceMeshShaderFeaturesEXT` | `D3D_SHADER_MODEL_6_5+` + `D3D12_FEATURE_D3D12_OPTIONS7` | Metal mesh shader（macOS 14+，`MTLGPUFamilyApple9+`）；`m_supportsMeshShaders=false` in stub | stub / unsupported (v2) |
| `ExtensionRayTracing` | `VK_KHR_ray_tracing_pipeline` + `VK_KHR_acceleration_structure` | `D3D12_FEATURE_DATA_D3D12_OPTIONS5`（raytracing tier 1+） | `MTLIntersectionFunctionTable`（Metal ray tracing，macOS 12+） | stub / unsupported (v2) |
| `DescriptorHeap` | Vulkan bindless pool（bindless descriptor indexing） | D3D12 shader-visible CBV/SRV/UAV heap（`m_cbvSrvUavHeap`） | Metal argument buffer tier 2（`m_supportsArgumentBuffers`） | stub / unsupported (v2) |
| `Residency` | Vulkan sparse binding / `VK_EXT_pageable_device_local_memory` | D3D12 tiled resources / `CreateResidencySet`（win32） | Metal heap residency（`MTLDevice.makeHeap`） | stub / unsupported (v2) |
| `PipelineCompiler` | Vulkan pipeline cache + async specialization compile | D3D12 PSO cache + DXC async compile | Metal binary archive + async MTLRenderPipelineState | stub / unsupported (v2) |
| `MultiQueue` | 独立 graphics/compute/transfer `VkQueue` | D3D12 multi-engine（graphics/compute/copy command queues） | Metal 每种 encoder 类型均支持；`MTLCommandQueue` 数量不限 | stub / unsupported (v2) |

### DeviceFeatureInfo 中 Vulkan 专有字段

`DeviceFeatureInfo` 当前含以下字段（`rhi/RHIDevice.h`）：
`timelineSemaphore`、`synchronization2`、`dynamicRendering`、`maintenance5`、`maintenance6`

**重要约束（T-06-14 mitigate）：**

| 字段 | Vulkan 对应 | D3D12 对应 | Metal 对应 | 跨后端适用性 |
|------|------------|-----------|-----------|------------|
| `timelineSemaphore` | `VkTimelineSemaphore`（VK_KHR_timeline_semaphore） | `ID3D12Fence`（SetEventOnCompletion/GetCompletedValue；无 timeline 概念，语义不同） | `MTLSharedEvent`（有限 timeline 语义） | **Vulkan-only 字段**：D3D12 / Metal 后端始终返回 `false`；上层代码不应假设非 Vulkan 后端此字段有意义 |
| `synchronization2` | `VK_KHR_synchronization2` extension | D3D12 enhanced barrier（Win11 22H2+；语义相近但非直接映射） | Metal 隐式同步（无对等概念） | **Vulkan-only 字段**：D3D12 / Metal 始终 `false` |
| `dynamicRendering` | `VK_KHR_dynamic_rendering` extension | D3D12 本身即无 render pass（天然动态）；此字段 N/A | Metal `MTLRenderPassDescriptor` 每帧动态创建；天然动态；此字段 N/A | Vulkan-specific：其他后端 `false`，但概念上两后端均"原生动态"，上层勿用此字段作为 D3D12/Metal 功能门 |
| `maintenance5` | `VK_KHR_maintenance5` extension | 无对等概念 | 无对等概念 | **Vulkan-only 字段**：始终 `false`（非 Vulkan） |
| `maintenance6` | `VK_KHR_maintenance6` extension | 无对等概念 | 无对等概念 | **Vulkan-only 字段**：始终 `false`（非 Vulkan） |

**结论：** `DeviceFeatureInfo` 中上述字段属 Vulkan 扩展特性暴露，不代表通用后端能力。
上层代码（render/ 目录）**不应**将这些字段用于 D3D12/Metal 后端的能力分支判断。
通用能力分支应使用 `supports(CapabilityTier)` / `CapabilityReport`，其字段均为
backend-neutral 布尔语义。

### DeviceCreateInfo（精简后）

经 Phase 6 Plan 02（D-08）下沉后，公共 `DeviceCreateInfo` 仅含 backend-neutral 字段：

| 字段 | 说明 | 三后端适用性 |
|-----|-----|------------|
| `CapabilityRequirements capabilityRequirements` | 设备创建时声明所需 capability tier | 全三后端适用 |
| `bool enableValidationLayers` | 是否启用验证层（调试用） | Vulkan: validation layers；D3D12: debug layer；Metal: Metal API validation |

Vulkan 专有字段（`instanceExtensions`、`deviceExtensions`、`instanceLayers`、`void* featuresStruct`）
已下沉到 `rhi/vulkan/VulkanDeviceCreateInfo`（backend-internal）；D3D12/Metal 路径不 include。

---

## BCK-03 结论

Phase 6 完成后，public RHI contract（`rhi/*.h` 公共头）不再被 Vulkan 专有语义锁死。
收口措施总结：

1. **Escape hatch 移除（D-07，Plan 03/03b）：** `getNativeHandle`、`getBackendDeviceHandle`、
   `resolve*BackendHandle` 等原属公共 `rhi::Device`/`rhi::Surface` 的 escape getter 已全部移除。
   合法 backend-internal interop（debug bridge、swapchain 构建所需 VkSurface/VkPhysicalDevice 传递）
   下沉到 `rhi/vulkan/VulkanDeviceInterop`，并记录进 allowlist；上层（render/ 目录）
   零 escape getter 调用（边界守卫 `native_getter: 0/0`，Phase 06-03b 已验证）。

2. **DeviceCreateInfo 精简（D-08，Plan 02）：** 公共 `DeviceCreateInfo` 已下沉 Vulkan 形状字段，
   仅保留 `CapabilityRequirements` 和 `enableValidationLayers`；D3D12/Metal 路径不涉及 Vulkan extension/layer 配置。

3. **VkImageLayout 封装（Barrier/Layout 节）：** `ResourceState` 枚举与 `VkImageLayout` 在
   语义上分离；VulkanDevice 内部负责 lowering，上层代码只操作可移植的 `ResourceState`。

4. **VkDescriptorSet 封装（Argument Table 节）：** `ArgumentTable`/`ArgumentWrite` 接口
   只接收 RHI handle；`VkDescriptorSet` / `VkDescriptorSetLayout` 完全在 VulkanDevice 内部管理。

5. **VkShaderModule 封装（Shader Module 节）：** `PipelineShaderStageDesc` 传 SPIR-V 字节码；
   VulkanDevice 内部创建和销毁 `VkShaderModule`（RDEV-02 已完成），上层不持有 VkShaderModule 句柄。

6. **统一 unsupported 路径（D-03，Plan 01）：** D3D12/Metal stub 的未实现方法统一调用
   `RHI_UNIMPLEMENTED`（LOGE + std::abort），消除裸 `assert(false)`，确保可审计性和一致性。

**BCK-03 风险来源：** 原 escape hatch（`resolveTextureBackendHandle` 等）允许上层直接拿到
`VkImage`，进而基于 `VkImageLayout` 假设操作；D-07 已通过 Plan 03/03b 将这些路径移入
`VulkanDeviceInterop`（backend-internal，allowlist 管控），上层无法再绕过 RHI 层直接操作
native 资源。BCK-03 风险源已消除。

---

## 编译门（机器可验证证据）

**D-10 证据 (a)：D3D12/Metal stub 编译通过**

D3D12Device（`rhi/d3d12/D3D12Device.h/.cpp`）和 MetalDevice（`rhi/metal/MetalDevice.h/.cpp`）
实现了 `rhi::Device` 的全部纯虚方法签名（`init`、`deinit`、`waitIdle`、`queryCapabilities`、
`supports`、`getGraphicsQueue` 等），cmake 构建零编译错误。

验证历程（Phase 06 累计构建结果）：

| Plan | 构建结果 | 关键变更 |
|------|---------|---------|
| 06-01 | BUILD_EXIT=0（5/5 targets） | D3D12/Metal stub RHI_UNIMPLEMENTED 注入 |
| 06-02 | BUILD_EXIT=0（5/5 targets） | VulkanDeviceInterop + DeviceCreateInfo 精简 |
| 06-03 | BUILD_EXIT=0（含 MSVC fix） | escape getter 签名移除 + render/ 调用点迁移 |
| 06-03b | BUILD_EXIT=0（6/6 targets） | DEMO_RENDER_RTV_BACKEND wrapper 删除 + baseline ratchet |

**边界守卫最终状态（06-03b 后）：**

| 守卫信号 | Phase 6 前 | Phase 6 后（06-03b） | 变化 |
|---------|-----------|---------------------|------|
| backend_include | 17 | 15 | -2 |
| vk_token | 865 | 522 | -343 |
| native_getter | 34 | 0 | **归零** |

```bash
# BCK-03 文档证据验证命令：
grep -c "^## Resource\|^## Pipeline\|^## Barrier\|^## Argument\|^## Shader\|^## Capability" docs/rhi-backend-mapping.md
# 期望输出：6

# BCK-01 + BCK-03 编译门验证（vcvars64 环境）：
cmake --build out/ 2>&1 | tail -5
# 期望：BUILD_EXIT=0，零编译错误

# escape getter 归零验证：
grep -rn "getBackendDeviceHandle\|resolve.*BackendHandle\|DEMO_RENDER_RTV_BACKEND" rhi/ render/
# 期望：零命中（VulkanDeviceInterop 内部 resolve* 不计入，属 rhi/vulkan/）
```

---

*本文档由 Phase 6 Plan 04（06-04）自动生成，基于 rhi/*.h 公共头实际代码状态。*
*如代码与文档存在分歧，以代码为准（T-06-13 要求）。*
