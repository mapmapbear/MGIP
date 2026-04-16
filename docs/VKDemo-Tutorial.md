# VKDemo 图形开发使用教程

> **目标读者**: 需要扩展渲染功能（如添加ShadowPass）的图形开发者
> **文档类型**: 实用指南 + 关键实现原理分析

---

## 1. 项目概览

### 1.1 架构图解

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application Layer                         │
│                    MinimalLatestApp.cpp                          │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                         Renderer                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │ PassExecutor│──│ Pass Nodes  │  │ Resource Management     │  │
│  │             │  │ - Compute   │  │ - HandlePool<T>         │  │
│  │             │  │ - Graphics  │  │ - TransientAllocator    │  │
│  │             │  │ - Present   │  │ - BindGroups            │  │
│  └─────────────┘  └─────────────┘  └─────────────────────────┘  │
└───────────────────────────┬─────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                          RHI Layer                               │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐             │
│  │ VulkanDevice │ │ MetalDevice  │ │ D3D12Device  │             │
│  │ VulkanCmdList│ │ MetalCmdList │ │ D3D12CmdList │             │
│  └──────────────┘ └──────────────┘ └──────────────┘             │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 核心模块关系

```
Renderer
    │
    ├── PassExecutor ──────────────── PassNode[] (AnimateVertices, SceneOpaque, Present, Imgui)
    │       │
    │       └── PassContext ───────── CommandList, TransientAllocator, RenderParams
    │
    ├── DrawStreamWriter ──────────── SceneOpaquePass 内部使用
    │       │
    │       └── DrawStream ────────── StreamEntry[] (setPipeline, setMaterial, draw...)
    │               │
    │               └── DrawStreamDecoder ── DecodedDraw[] ── Renderer::recordGraphicCommands
    │
    ├── TransientAllocator ────────── 每帧临时数据 (SceneInfo UBO)
    │
    └── BindGroups ────────────────── materialBindGroup, sceneBindGroup (per-frame)
```

### 1.3 资源生命周期层次

| 层次 | 生命周期 | 资源类型 | 触发条件 |
|------|----------|----------|----------|
| **Device Lifetime** | 整个设备存在期间 | Context, Allocator, Pipelines, 持久Buffer | Renderer::init() 创建, shutdown() 销毁 |
| **Swapchain Lifetime** | Swapchain有效期间 | Swapchain images, GBuffer | 窗口resize/vSync切换触发重建 |
| **Per-Frame Lifetime** | 每帧循环 | CommandPool, CommandBuffer, TransientAllocator | 帧环复用, 每帧reset |
| **Per-Pass Lifetime** | 每个Pass执行期间 | DrawStream, DecodedDraws | 每帧清空 |

---

## 2. Pass系统

### 2.1 PassNode接口与生命周期

**接口定义** (`render/Pass.h`):

```cpp
class PassNode {
public:
    virtual ~PassNode() = default;

    // Pass名称 (用于调试/Profiling)
    [[nodiscard]] virtual const char* getName() const = 0;

    // 声明资源依赖 (用于自动Barrier插入)
    [[nodiscard]] virtual HandleSlice<PassResourceDependency> getDependencies() const = 0;

    // 执行Pass逻辑
    virtual void execute(const PassContext& context) const = 0;
};

// 具体类型标记
class ComputePassNode : public PassNode {};
class RenderPassNode : public PassNode {};
```

**生命周期流程**:

```
Renderer::init()
    │
    ├── 创建Pass实例 (make_unique<SceneOpaquePass>(this))
    │
    └── 注册到PassExecutor (m_passExecutor.addPass(*m_sceneOpaquePass))

每帧 Renderer::drawFrame()
    │
    ├── 准备PassContext (cmd, transientAllocator, params, drawStream)
    │
    └── PassExecutor::execute(context)
            │
            ├── 遍历所有注册的Pass
            │       │
            │       ├── 查询 getDependencies()
            │       │
            │       ├── 插入资源Barrier (根据依赖关系自动计算)
            │       │
            │       └── 调用 pass->execute(context)
            │
            └── 结束时转换Swapchain到Present状态
```

### 2.2 PassContext与依赖声明

**PassContext结构** (`render/Pass.h:17-27`):

```cpp
struct PassContext {
    rhi::CommandList*   cmd;                // 当前帧的命令列表
    TransientAllocator* transientAllocator; // 每帧临时分配器
    uint32_t            frameIndex;         // 当前帧索引
    uint32_t            passIndex;          // 当前Pass索引
    const RenderParams* params;             // 渲染参数 (viewport, deltaTime等)
    std::vector<StreamEntry>* drawStream;   // DrawStream输出 (仅Graphics Pass)
};
```

**依赖声明示例** (`render/passes/SceneOpaquePass.cpp:15-23`):

```cpp
PassNode::HandleSlice<PassResourceDependency> SceneOpaquePass::getDependencies() const {
    static const std::array<PassResourceDependency, 3> dependencies = {
        // VertexBuffer: 顶点着色器读取
        PassResourceDependency::buffer(
            kPassVertexBufferHandle,
            ResourceAccess::read,
            rhi::ShaderStage::vertex
        ),
        // GBuffer Color: 片段着色器写入
        PassResourceDependency::texture(
            kPassGBufferColorHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
        // GBuffer Depth: 片段着色器写入
        PassResourceDependency::texture(
            kPassGBufferDepthHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}
```

### 2.3 现有Pass实现解读

**Compute Pass示例** - `AnimateVerticesPass`:

```cpp
void AnimateVerticesPass::execute(const PassContext& context) const {
    context.cmd->beginEvent("AnimateVertices");  // Profiling标记

    // 直接委托给Renderer的Compute执行
    m_renderer->executeComputePass(*context.cmd, *context.params);

    context.cmd->endEvent();
}
```

**Graphics Pass示例** - `SceneOpaquePass`:

```cpp
void SceneOpaquePass::execute(const PassContext& context) const {
    context.cmd->beginEvent("SceneOpaque");

    // 1. 创建DrawStreamWriter
    DrawStreamWriter writer{};
    writer.clear();

    // 2. 设置绘制状态
    writer.setPipeline(m_renderer->getGraphicsPipelineHandle(GraphicsPipelineVariant::nonTextured));
    writer.setMaterialIndex(materialIndex);
    writer.setMesh(kNullMeshHandle);
    writer.setDynamicBufferIndex(sceneIndex);
    writer.setDynamicOffset(m_renderer->allocateDrawDynamicOffset(materialIndex, *context.params));

    // 3. 发出绘制命令
    writer.draw(0, 3, 1);  // vertexOffset=0, vertexCount=3, instanceCount=1

    // 4. 获取DrawStream并执行Graphics Pass
    std::vector<StreamEntry>& drawStream = *context.drawStream;
    drawStream = writer.entries();
    m_renderer->executeGraphicsPass(*context.cmd, *context.params, drawStream);

    context.cmd->endEvent();
}
```

### 2.4 实现原理：PassExecutor调度

**核心调度逻辑** (`render/PassExecutor.cpp:153-274`):

```cpp
void PassExecutor::execute(const PassContext& context) const {
    // 1. 初始化资源状态追踪
    std::unordered_map<uint64_t, BufferUsageState>  bufferStates;
    std::unordered_map<uint64_t, TextureUsageState> textureStates;

    // 2. 设置初始资源状态
    for (const TextureBinding& binding : m_textureBindings) {
        textureStates[toHandleKey(binding.handle)] = TextureUsageState{...};
        context.cmd->setResourceState(binding.handle, binding.initialState);
    }

    // 3. 按顺序执行每个Pass
    for (uint32_t passIndex = 0; passIndex < m_passes.size(); ++passIndex) {
        const PassNode* pass = m_passes[passIndex];

        // 3.1 分析Pass的依赖声明
        auto dependencies = pass->getDependencies();

        // 3.2 为每个依赖插入Barrier
        for (const auto& dep : dependencies) {
            // 检查是否需要Barrier
            if (requiresBarrier(previousAccess, dep.access)) {
                // 插入Buffer/Texture Barrier
                context.cmd->transitionBuffer(...);
                // 或
                context.cmd->transitionTexture(...);
            }

            // 更新资源状态追踪
            resourceStates[handle] = newState;
        }

        // 3.3 执行Pass
        PassContext scopedContext = context;
        scopedContext.passIndex = passIndex;
        pass->execute(scopedContext);
    }

    // 4. 最终转换Swapchain到Present状态
    for (const TextureBinding& binding : m_textureBindings) {
        if (binding.isSwapchain) {
            context.cmd->transitionTexture(/* to Present state */);
        }
    }
}
```

**Barrier决策原理**:

```
requiresBarrier(previousAccess, nextAccess):
    return !(previousAccess == read && nextAccess == read)

// 即：只有连续的read-read不需要Barrier
// read->write, write->read, write->write 都需要Barrier
```

### 2.5 使用模式：添加新Pass

**步骤1**: 创建Pass类文件

```cpp
// render/passes/ShadowPass.h
#pragma once
#include "../Pass.h"

namespace demo {

class ShadowPass : public RenderPassNode {
public:
    explicit ShadowPass(Renderer* renderer);
    ~ShadowPass() override = default;

    [[nodiscard]] const char* getName() const override { return "ShadowPass"; }
    [[nodiscard]] HandleSlice<PassResourceDependency> getDependencies() const override;
    void execute(const PassContext& context) const override;

private:
    Renderer* m_renderer;
};

}  // namespace demo
```

**步骤2**: 实现依赖声明

```cpp
// render/passes/ShadowPass.cpp
PassNode::HandleSlice<PassResourceDependency> ShadowPass::getDependencies() const {
    static const std::array<PassResourceDependency, 2> dependencies = {
        // 深度纹理写入
        PassResourceDependency::texture(
            kPassShadowMapHandle,     // 需要定义新的pseudo-handle
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
        // 场景几何读取
        PassResourceDependency::buffer(
            kPassVertexBufferHandle,
            ResourceAccess::read,
            rhi::ShaderStage::vertex
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}
```

**步骤3**: 实现execute方法

```cpp
void ShadowPass::execute(const PassContext& context) const {
    context.cmd->beginEvent("Shadow");

    // 使用DrawStreamWriter录制阴影绘制
    DrawStreamWriter writer{};
    writer.clear();

    // 设置阴影Pipeline
    writer.setPipeline(m_renderer->getGraphicsPipelineHandle(GraphicsPipelineVariant::shadow));
    // ... 设置其他状态

    writer.draw(...);

    // 获取DrawStream并执行
    std::vector<StreamEntry>& drawStream = *context.drawStream;
    drawStream = writer.entries();
    m_renderer->executeGraphicsPass(*context.cmd, *context.params, drawStream);

    context.cmd->endEvent();
}
```

**步骤4**: 注册Pass

```cpp
// render/Renderer.cpp 的 init() 中
m_shadowPass = std::make_unique<ShadowPass>(this);
m_passExecutor.addPass(*m_shadowPass);  // 按顺序添加
```

---

## 3. RHI抽象层

### 3.1 RHI接口设计理念

**目标**: 提供跨平台(Vulkan/Metal/D3D12)的统一渲染接口

**设计原则**:
- **Handle-based**: 所有资源通过类型化Handle访问，隐藏平台细节
- **CommandList抽象**: 统一的命令录制接口
- **平台特定实现**: 通过继承实现各平台差异

**Handle类型** (`rhi/RHIHandles.h`):

```cpp
struct BufferHandle  { uint32_t index; uint32_t generation; };
struct TextureHandle { uint32_t index; uint32_t generation; };
struct PipelineHandle { uint32_t index; uint32_t generation; };
struct BindTableHandle { uint32_t index; uint32_t generation; };
```

### 3.2 CommandList核心操作

**接口定义** (`rhi/RHICommandList.h`):

```cpp
class CommandList {
public:
    virtual ~CommandList() = default;

    // 生命周期
    virtual void begin() = 0;
    virtual void end() = 0;

    // RenderPass
    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void endRenderPass() = 0;

    // 状态设置
    virtual void setViewport(const Viewport& viewport) = 0;
    virtual void setScissor(const Rect2D& scissor) = 0;

    // 资源转换
    virtual void setResourceState(ResourceHandle resource, ResourceState state) = 0;
    virtual void insertBarrier(BarrierType barrierType) = 0;
    virtual void transitionBuffer(const BufferBarrierDesc& desc) = 0;
    virtual void transitionTexture(const TextureBarrierDesc& desc) = 0;

    // 绑定操作
    virtual void bindPipeline(PipelineBindPoint bindPoint, PipelineHandle pipeline) = 0;
    virtual void bindBindTable(PipelineBindPoint bindPoint, uint32_t slot,
                               BindTableHandle bindTable,
                               const uint32_t* dynamicOffsets,
                               uint32_t dynamicOffsetCount) = 0;
    virtual void bindVertexBuffers(uint32_t firstBinding,
                                   const BufferHandle* buffers,
                                   const uint64_t* offsets,
                                   uint32_t bufferCount) = 0;

    // Push Constants
    virtual void pushConstants(ShaderStage stages,
                               uint32_t offset, uint32_t size,
                               const void* data) = 0;

    // 绘制/计算
    virtual void draw(uint32_t vertexCount, uint32_t instanceCount,
                      uint32_t firstVertex, uint32_t firstInstance) = 0;
    virtual void dispatch(uint32_t groupCountX, uint32_t groupCountY,
                          uint32_t groupCountZ) = 0;

    // Profiling标记
    virtual void beginEvent(const char* name) = 0;
    virtual void endEvent() = 0;
};
```

### 3.3 资源Handle与Pool模型

**HandlePool模板** (`common/HandlePool.h`):

```cpp
template <typename HandleT, typename RecordT>
class HandlePool {
public:
    // 创建新记录，返回Handle
    HandleT emplace(RecordT record);

    // 查找记录 (支持generation校验)
    RecordT* tryGet(HandleT handle);
    const RecordT* tryGet(HandleT handle) const;

    // 销毁记录
    bool destroy(HandleT handle);

    // 遍历所有活跃记录
    template <typename Fn>
    void forEachActive(Fn&& fn);
};
```

**使用示例** (Renderer内部):

```cpp
// Pipeline注册
PipelineHandle handle = m_device.pipelineRegistry.emplace(PipelineRecord{
    .bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    .nativePipeline = reinterpret_cast<uint64_t>(vkPipeline),
    .specializationVariant = variant
});

// Pipeline查找
const PipelineRecord* record = m_device.pipelineRegistry.tryGet(handle);
if (record && record->nativePipeline != 0) {
    // 使用pipeline
}
```

### 3.4 跨后端实现差异

**Vulkan实现** (`rhi/vulkan/VulkanCommandList.cpp`):

```cpp
void VulkanCommandList::bindPipeline(PipelineBindPoint bindPoint, PipelineHandle pipeline) {
    VkPipelineBindPoint vkBindPoint = static_cast<VkPipelineBindPoint>(bindPoint);
    VkPipeline vkPipeline = reinterpret_cast<VkPipeline>(pipeline);
    vkCmdBindPipeline(m_commandBuffer, vkBindPoint, vkPipeline);
}
```

**Metal实现** (`rhi/metal/MetalCommandList.cpp`):

```cpp
void MetalCommandList::bindPipeline(PipelineBindPoint bindPoint, PipelineHandle pipeline) {
    id<MTLRenderCommandEncoder> encoder = getActiveEncoder();
    id<MTLRenderPipelineState> pso = reinterpret_cast<id<MTLRenderPipelineState>>(pipeline);
    [encoder setRenderPipelineState:pso];
}
```

**D3D12实现** (`rhi/d3d12/D3D12CommandList.cpp`):

```cpp
void D3D12CommandList::bindPipeline(PipelineBindPoint bindPoint, PipelineHandle pipeline) {
    ID3D12PipelineState* pso = reinterpret_cast<ID3D12PipelineState*>(pipeline);
    m_commandList->SetPipelineState(pso);
}
```

### 3.5 使用模式：RHI命令录制

**在Pass中使用CommandList**:

```cpp
void MyPass::execute(const PassContext& context) const {
    rhi::CommandList& cmd = *context.cmd;

    // 1. 开始Profiling标记
    cmd.beginEvent("MyPass");

    // 2. 资源转换
    cmd.transitionTexture(rhi::TextureBarrierDesc{
        .texture = myTexture,
        .nativeImage = nativeImageHandle,
        .aspect = rhi::TextureAspect::color,
        .srcStage = rhi::PipelineStage::Compute,
        .dstStage = rhi::PipelineStage::FragmentShader,
        .oldState = rhi::ResourceState::general,
        .newState = rhi::ResourceState::shaderRead
    });

    // 3. 开始RenderPass
    cmd.beginRenderPass(rhi::RenderPassDesc{
        .renderArea = {0, 0, width, height},
        .colorTargets = &colorTarget,
        .colorTargetCount = 1,
        .depthTarget = &depthTarget
    });

    // 4. 设置状态
    cmd.setViewport({0, 0, float(width), float(height), 0, 1});
    cmd.setScissor({0, 0, width, height});

    // 5. 绑定资源
    cmd.bindPipeline(rhi::PipelineBindPoint::graphics, pipelineHandle);
    cmd.bindBindTable(rhi::PipelineBindPoint::graphics, 0, bindTableHandle, nullptr, 0);

    // 6. 绘制
    cmd.draw(vertexCount, instanceCount, 0, 0);

    // 7. 结束RenderPass
    cmd.endRenderPass();

    cmd.endEvent();
}
```

---

## 4. DrawStream系统

### 4.1 StreamEntry类型与编码

**Entry类型** (`render/DrawStream.h`):

```cpp
enum class StreamEntryType : uint8_t {
    setPipeline      = 0,  // 设置Pipeline
    setMaterial      = 1,  // 设置Material索引
    setMesh          = 2,  // 设置Mesh Handle
    setDynamicBuffer = 3,  // 设置动态UBO Buffer索引
    setDynamicOffset = 4,  // 设置动态UBO偏移
    draw             = 5,  // 发出绘制命令
};

struct StreamEntry {
    StreamEntryType type;
    union {
        PipelineHandle pipeline;
        rhi::ResourceIndex materialIndex;
        MeshHandle mesh;
        rhi::ResourceIndex dynamicBufferIndex;
        uint32_t dynamicOffset;
        struct {
            uint32_t dirtyMask;      // 标记哪些状态变化了
            uint32_t vertexOffset;
            uint32_t vertexCount;
            uint32_t instanceCount;
        } draw;
    } payload;
};
```

**编码格式特点**:
- **状态变更与绘制分离**: 设置状态的Entry和draw Entry分开
- **脏标记优化**: draw Entry携带dirtyMask，标记自上次draw以来的状态变化
- **紧凑存储**: 使用union减少内存占用

### 4.2 DrawStreamWriter状态机

**Writer状态** (`render/DrawStreamWriter.h`):

```cpp
class DrawStreamWriter {
public:
    struct State {
        PipelineHandle pipeline;
        rhi::ResourceIndex materialIndex;
        MeshHandle mesh;
        rhi::ResourceIndex dynamicBufferIndex;
        uint32_t dynamicOffset;
    };

    void clear();                    // 重置状态
    void setPipeline(PipelineHandle);
    void setMaterialIndex(rhi::ResourceIndex);
    void setMesh(MeshHandle);
    void setDynamicBufferIndex(rhi::ResourceIndex);
    void setDynamicOffset(uint32_t);
    void draw(vertexOffset, vertexCount, instanceCount);

    const DrawStream& entries() const;  // 获取编码结果

private:
    State m_state;           // 当前状态
    State m_lastEmittedState; // 上次发射时的状态
    bool m_hasEmittedDraw;   // 是否已发射过draw
    DrawStream m_entries;    // 编码结果
};
```

**状态转换图**:

```
              ┌──────────────────────────────────────────┐
              │              clear()                      │
              │  m_state = {}, m_lastEmittedState = {}    │
              │  m_hasEmittedDraw = false                 │
              └───────────────────┬──────────────────────┘
                                  │
              ┌───────────────────▼──────────────────────┐
              │            set*() 方法                    │
              │  只更新 m_state，不产生Entry             │
              └───────────────────┬──────────────────────┘
                                  │
              ┌───────────────────▼──────────────────────┐
              │              draw()                       │
              │  1. 调用 emitCurrentState()              │
              │     - 比较 m_state vs m_lastEmittedState │
              │     - 只发射变化的Entry                  │
              │  2. 发射 draw Entry                      │
              │  3. 更新 m_lastEmittedState              │
              └───────────────────────────────────────────┘
```

**emitCurrentState实现** (`render/DrawStreamWriter.cpp:61-109`):

```cpp
uint32_t DrawStreamWriter::emitCurrentState() {
    uint32_t dirtyMask = 0;

    // 首次draw或Pipeline变化
    if (!m_hasEmittedDraw || !(m_state.pipeline == m_lastEmittedState.pipeline)) {
        emit(StreamEntry{.type = setPipeline, .payload.pipeline = m_state.pipeline});
        dirtyMask |= kDrawStreamDirtyPipeline;
    }

    // Material变化
    if (!m_hasEmittedDraw || m_state.materialIndex != m_lastEmittedState.materialIndex) {
        emit(StreamEntry{.type = setMaterial, .payload.materialIndex = m_state.materialIndex});
        dirtyMask |= kDrawStreamDirtyMaterial;
    }

    // ... 其他状态类似

    m_lastEmittedState = m_state;
    m_hasEmittedDraw = true;
    return dirtyMask;
}
```

### 4.3 DrawStreamDecoder解码流程

**Decoder状态机** (`render/DrawStreamDecoder.h`):

```cpp
class DrawStreamDecoder {
public:
    struct State {
        PipelineHandle pipeline;
        rhi::ResourceIndex materialIndex;
        MeshHandle mesh;
        rhi::ResourceIndex dynamicBufferIndex;
        uint32_t dynamicOffset;
    };

    struct DecodedDraw {
        State state;            // 绘制时的完整状态快照
        uint32_t vertexOffset;
        uint32_t vertexCount;
        uint32_t instanceCount;
    };

    bool decode(const DrawStream& stream, std::vector<DecodedDraw>& outDraws) const;
};
```

**解码流程**:

```
输入: DrawStream [setPipeline, setMaterial, setDynamicOffset, draw, setPipeline, draw]

解码过程:
    State currentState = {};

    for (entry in stream):
        switch (entry.type):
            case setPipeline:
                currentState.pipeline = entry.payload.pipeline
            case setMaterial:
                currentState.materialIndex = entry.payload.materialIndex
            case setDynamicOffset:
                currentState.dynamicOffset = entry.payload.dynamicOffset
            case draw:
                // 验证所有必需状态已设置
                assert(currentState.pipeline.valid)
                assert(currentState.dynamicOffset != INVALID)

                // 输出DecodedDraw
                outDraws.push(DecodedDraw{
                    .state = currentState,  // 快照
                    .vertexOffset = entry.payload.draw.vertexOffset,
                    .vertexCount = entry.payload.draw.vertexCount,
                    .instanceCount = entry.payload.draw.instanceCount
                })

输出: DecodedDraw[] (每个draw都有完整状态快照)
```

### 4.4 实现原理：脏标记优化

**脏标记常量** (`render/DrawStream.h:23-27`):

```cpp
constexpr uint32_t kDrawStreamDirtyPipeline      = 1u << 0u;
constexpr uint32_t kDrawStreamDirtyMaterial      = 1u << 1u;
constexpr uint32_t kDrawStreamDirtyMesh          = 1u << 2u;
constexpr uint32_t kDrawStreamDirtyDynamicBuffer = 1u << 3u;
constexpr uint32_t kDrawStreamDirtyDynamicOffset = 1u << 4u;
```

**优化效果示例**:

```cpp
// 编码端
writer.setPipeline(pipelineA);
writer.setMaterial(materialA);
writer.setDynamicOffset(offsetA);
writer.draw(0, 3, 1);  // dirtyMask = ALL

writer.setDynamicOffset(offsetB);  // 只改offset
writer.draw(3, 3, 1);  // dirtyMask = DYNAMIC_OFFSET_ONLY

// 生成的Stream:
// [setPipeline, setMaterial, setDynamicOffset, draw(dirty=ALL),
//  setDynamicOffset, draw(dirty=OFFSET)]

// 解码端可以根据dirtyMask优化:
// - 第一个draw需要绑定所有资源
// - 第二个draw只需要更新dynamicOffset
```

### 4.5 使用模式：绘制命令录制

**在SceneOpaquePass中的典型用法**:

```cpp
void SceneOpaquePass::execute(const PassContext& context) const {
    DrawStreamWriter writer{};
    writer.clear();

    // 绘制第一个三角形 (无纹理)
    writer.setPipeline(m_renderer->getGraphicsPipelineHandle(GraphicsPipelineVariant::nonTextured));
    writer.setMaterialIndex(materialIndex);
    writer.setMesh(kNullMeshHandle);
    writer.setDynamicBufferIndex(sceneIndex);
    writer.setDynamicOffset(m_renderer->allocateDrawDynamicOffset(materialIndex, *context.params));
    writer.draw(0, 3, 1);  // vertexOffset=0, 3个顶点

    // 绘制第二个三角形 (有纹理) - 只改变Pipeline和Offset
    writer.setPipeline(m_renderer->getGraphicsPipelineHandle(GraphicsPipelineVariant::textured));
    writer.setDynamicOffset(m_renderer->allocateDrawDynamicOffset(materialIndex, *context.params));
    writer.draw(3, 3, 1);  // vertexOffset=3, 3个顶点

    // 获取DrawStream
    *context.drawStream = writer.entries();

    // 执行Graphics Pass
    m_renderer->executeGraphicsPass(*context.cmd, *context.params, *context.drawStream);
}
```

---

## 5. BindGroups与资源绑定

### 5.1 BindGroupSetSlot约定

**Slot定义** (`render/BindGroups.h`):

```cpp
enum class BindGroupSetSlot : uint32_t {
    passGlobals    = 0,  // Pass全局资源 (保留)
    material       = 1,  // 材质纹理组
    drawDynamic    = 2,  // 每帧动态UBO
    shaderSpecific = 3,  // Shader特定资源
};

constexpr uint32_t kBindGroupSetCount = 4;
```

**Slot到Shader Set的映射** (`render/Renderer.cpp:297-308`):

```cpp
std::optional<uint32_t> Renderer::mapSetSlotToLegacyShaderSet(BindGroupSetSlot slot) {
    switch (slot) {
        case BindGroupSetSlot::material:
            return shaderio::LSetTextures;  // Set 1
        case BindGroupSetSlot::drawDynamic:
            return shaderio::LSetScene;     // Set 2
        default:
            return std::nullopt;
    }
}
```

### 5.2 BindGroupDesc定义

**描述结构** (`render/BindGroups.h:38-45`):

```cpp
struct BindGroupDesc {
    BindGroupSetSlot      slot;               // Set Slot
    rhi::BindTableLayout* layout;             // 布局
    rhi::BindTable*       table;              // 描述符表
    rhi::ResourceIndex    primaryLogicalIndex; // 主逻辑索引
    const char*           debugName;          // 调试名称
};
```

**创建示例** (`render/Renderer.cpp:1298-1323`):

```cpp
// Material BindGroup (Set 1)
{
    std::vector<rhi::BindTableLayoutEntry> layoutEntries{
        rhi::BindTableLayoutEntry{
            .logicalIndex    = kMaterialBindlessTexturesIndex,  // 0
            .resourceType    = rhi::BindlessResourceType::sampledTexture,
            .descriptorCount = maxTextures,
            .visibility      = rhi::ResourceVisibility::allGraphics,
        }
    };

    auto* materialLayout = new rhi::vulkan::VulkanBindTableLayout();
    materialLayout->init(device, layoutEntries);

    auto* materialTable = new rhi::vulkan::VulkanBindTable();
    materialTable->init(device, *materialLayout, maxTextures);

    BindGroupDesc materialBindGroupDesc{
        .slot                = BindGroupSetSlot::material,
        .layout              = materialLayout,
        .table               = materialTable,
        .primaryLogicalIndex = kMaterialBindlessTexturesIndex,
        .debugName           = "material-texture-bind-group",
    };
    m_materials.materialBindGroup = createBindGroup(std::move(materialBindGroupDesc));
}

// Per-Frame Scene BindGroup (Set 2)
{
    for (uint32_t i = 0; i < frameCount; ++i) {
        auto* sceneLayout = new rhi::vulkan::VulkanBindTableLayout();
        sceneLayout->init(device, layoutEntries);

        auto* sceneTable = new rhi::vulkan::VulkanBindTable();
        sceneTable->init(device, *sceneLayout, 1);

        BindGroupDesc sceneBindGroupDesc{
            .slot                = BindGroupSetSlot::drawDynamic,
            .layout              = sceneLayout,
            .table               = sceneTable,
            .primaryLogicalIndex = kSceneBindlessInfoIndex,
            .debugName           = "scene-dynamic-bind-group",
        };
        m_perFrame.frameUserData[i].sceneBindGroup = createBindGroup(std::move(sceneBindGroupDesc));
    }
}
```

### 5.3 Descriptor生命周期

**创建流程**:

```
Renderer::init()
    │
    ├── createDescriptorPool()     // 创建全局Descriptor Pool
    │
    ├── createGraphicDescriptorSet()
    │       │
    │       ├── 创建 Material BindGroup (全局)
    │       │
    │       └── 创建 Scene BindGroup (per-frame)
    │
    └── updateGraphicsDescriptorSet()  // 写入初始数据
```

**更新流程** (`render/Renderer.cpp:1353-1431`):

```cpp
void Renderer::updateGraphicsDescriptorSet() {
    // 1. 获取Sampler
    VkSampler sampler = m_device.samplerPool.acquireSampler({...});

    // 2. 准备Image信息
    std::array<VkDescriptorImageInfo, kDemoMaterialSlotCount> imageInfos{};
    for (uint32_t slot = 0; slot < kDemoMaterialSlotCount; ++slot) {
        const auto* material = tryGetMaterial(m_materials.sampleMaterials[slot]);
        const auto* textureHot = tryGetTextureHot(material->sampledTexture);

        imageInfos[material->descriptorIndex] = {
            .sampler     = sampler,
            .imageView   = textureHot->sampledImageView,
            .imageLayout = textureHot->sampledImageLayout,
        };
    }

    // 3. 写入Material BindGroup
    const rhi::BindTableWrite materialWrite{
        .dstIndex        = kMaterialBindlessTexturesIndex,
        .dstArrayElement = 0,
        .resourceType    = rhi::BindlessResourceType::sampledTexture,
        .descriptorCount = imageInfos.size(),
        .pImageInfo      = descriptorImageInfos.data(),
    };
    updateBindGroup(m_materials.materialBindGroup, &materialWrite, 1);

    // 4. 写入Per-Frame Scene BindGroup
    for (auto& frameUserData : m_perFrame.frameUserData) {
        const rhi::BindTableWrite sceneWrite{
            .dstIndex     = kSceneBindlessInfoIndex,
            .resourceType = rhi::BindlessResourceType::uniformBuffer,
            .pBufferInfo  = &sceneBufferWrite,
        };
        updateBindGroup(frameUserData.sceneBindGroup, &sceneWrite, 1);
    }
}
```

### 5.4 实现原理：Bindless架构

**Bindless核心概念**:

```
传统绑定模型:
    vkCmdBindDescriptorSets(cmd, set=0, descriptorSet=materialDS0);
    draw();
    vkCmdBindDescriptorSets(cmd, set=0, descriptorSet=materialDS1);
    draw();

Bindless模型:
    vkCmdBindDescriptorSets(cmd, set=1, descriptorSet=allMaterialsDS);  // 一次性绑定所有纹理
    draw(materialIndex=0);  // Shader通过索引访问
    draw(materialIndex=1);  // Shader通过索引访问
```

**Shader侧访问** (`shaderio`):

```glsl
// Bindless纹理数组
layout(set = 1, binding = 0) uniform texture2D textures[];
layout(set = 1, binding = 0) uniform sampler samplers[];

// 通过索引访问
vec4 color = texture(sampler2D(textures[materialIndex], samplers[0]), uv);
```

**优势**:
- 减少Descriptor Set切换
- 支持大量纹理
- 适合DrawStream批量绘制

### 5.5 使用模式：创建/更新绑定组

**创建新BindGroup**:

```cpp
// 1. 定义布局
std::vector<rhi::BindTableLayoutEntry> layoutEntries = {
    rhi::BindTableLayoutEntry{
        .logicalIndex    = 0,
        .resourceType    = rhi::BindlessResourceType::sampledTexture,
        .descriptorCount = 10,
        .visibility      = rhi::ResourceVisibility::allGraphics,
    },
    rhi::BindTableLayoutEntry{
        .logicalIndex    = 1,
        .resourceType    = rhi::BindlessResourceType::uniformBuffer,
        .descriptorCount = 1,
        .visibility      = rhi::ResourceVisibility::allGraphics,
    },
};

// 2. 创建Layout和Table
auto* layout = new rhi::vulkan::VulkanBindTableLayout();
layout->init(device, layoutEntries);

auto* table = new rhi::vulkan::VulkanBindTable();
table->init(device, *layout, totalDescriptors);

// 3. 注册BindGroup
BindGroupDesc desc{
    .slot                = BindGroupSetSlot::shaderSpecific,
    .layout              = layout,
    .table               = table,
    .primaryLogicalIndex = 0,
    .debugName           = "my-bind-group",
};
BindGroupHandle handle = createBindGroup(std::move(desc));
```

**更新BindGroup**:

```cpp
// 写入纹理
rhi::DescriptorImageInfo imageInfo = {
    .sampler     = samplerHandle,
    .imageView   = imageViewHandle,
    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
};

rhi::BindTableWrite write = {
    .dstIndex        = 0,  // logical index
    .dstArrayElement = 0,  // array element
    .resourceType    = rhi::BindlessResourceType::sampledTexture,
    .descriptorCount = 1,
    .pImageInfo      = &imageInfo,
};

updateBindGroup(handle, &write, 1);
```

---

## 6. TransientAllocator

### 6.1 线性分配原理

**设计目标**: 为每帧动态数据(如SceneInfo UBO)提供高效的临时内存分配

**核心结构** (`render/TransientAllocator.h`):

```cpp
class TransientAllocator {
public:
    struct Allocation {
        void*        cpuPtr;      // CPU映射指针
        BufferHandle handle;      // Buffer Handle
        uint32_t     offset;      // 在Buffer中的偏移
    };

    void init(rhi::Device& device, VmaAllocator allocator, uint32_t bufferSize);
    Allocation allocate(uint32_t size, uint32_t alignment);
    void flushAllocation(const Allocation& allocation, uint32_t size) const;
    void reset();  // 每帧开始时调用
    void destroy();

private:
    VkDevice      m_device;
    VmaAllocator  m_allocator;
    utils::Buffer m_buffer;       // 单个大Buffer
    void*         m_mappedData;   // 持久映射的CPU指针
    bool          m_isHostCoherent;
    uint32_t      m_capacity;
    uint32_t      m_head;         // 当前分配位置
};
```

**线性分配算法**:

```
初始状态:
    [________________________________________________]  m_capacity = 1MB
    ^
    m_head = 0

分配 alloc(256, 256):
    [256B aligned|_______________________________]
                 ^
                 m_head = 256

分配 alloc(512, 16):
    [256B aligned|512B|__________________________]
                      ^
                      m_head = 768

reset():
    [________________________________________________]
    ^
    m_head = 0
```

### 6.2 使用模式：每帧临时数据

**初始化** (`render/Renderer.cpp:677-680`):

```cpp
// 每帧创建一个TransientAllocator
for (auto& frameUserData : m_perFrame.frameUserData) {
    frameUserData.transientAllocator.init(
        *m_device.device,
        m_device.allocator,
        kPerFrameTransientAllocatorSize  // 1MB
    );
}
```

**每帧使用** (`render/Renderer.cpp:882-908`):

```cpp
uint32_t Renderer::allocateDrawDynamicOffset(rhi::ResourceIndex materialIndex, const RenderParams& params) {
    // 1. 计算对齐要求
    const uint32_t dynamicAlignment = alignUp(
        sizeof(shaderio::SceneInfo),
        deviceProperties.limits.minUniformBufferOffsetAlignment
    );

    // 2. 准备数据
    shaderio::SceneInfo sceneInfo{};
    sceneInfo.animValue = ...;
    sceneInfo.dataBufferAddress = m_device.pointsBuffer.address;
    // ...

    // 3. 从TransientAllocator分配
    const uint32_t currentFrameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
    TransientAllocator& allocator = m_perFrame.frameUserData[currentFrameIndex].transientAllocator;

    const auto allocation = allocator.allocate(sizeof(sceneInfo), dynamicAlignment);

    // 4. 写入数据
    std::memcpy(allocation.cpuPtr, &sceneInfo, sizeof(sceneInfo));
    allocator.flushAllocation(allocation, sizeof(sceneInfo));

    // 5. 返回偏移 (用于Dynamic UBO)
    return allocation.offset;
}
```

**每帧重置** (`render/Renderer.cpp:694`):

```cpp
bool Renderer::prepareFrameResources() {
    // ...
    const uint32_t currentFrameIndex = m_perFrame.frameContext->getCurrentFrameIndex();
    m_perFrame.frameUserData[currentFrameIndex].transientAllocator.reset();  // 重置为0
    // ...
}
```

### 6.3 与DrawStream配合

**数据流**:

```
SceneOpaquePass::execute()
    │
    ├── allocateDrawDynamicOffset()
    │       │
    │       ├── TransientAllocator::allocate() ── 返回 offset
    │       │
    │       └── 写入 SceneInfo 到 allocation.cpuPtr
    │
    ├── DrawStreamWriter::setDynamicOffset(offset)
    │
    └── DrawStreamWriter::draw()
            │
            └── DrawStream 包含 dynamicOffset

Renderer::recordGraphicCommands()
    │
    ├── DrawStreamDecoder::decode() ── DecodedDraw.state.dynamicOffset
    │
    └── vkCmdBindDescriptorSets(..., &dynamicOffset)  // 绑定动态UBO偏移
```

**关键点**:
- TransientAllocator提供**线性分配**，O(1)时间
- 每帧reset()，无需单独释放
- 配合Dynamic UBO实现**每Draw唯一数据**

---

## 附录A：关键文件索引

| 文件路径 | 主要内容 |
|----------|----------|
| `render/Renderer.h` | Renderer类定义，资源所有权 |
| `render/Renderer.cpp` | Renderer实现，初始化/渲染流程 |
| `render/Pass.h` | PassNode接口，PassContext定义 |
| `render/PassExecutor.cpp` | Pass调度，自动Barrier插入 |
| `render/DrawStream.h` | StreamEntry定义，DrawStream类型 |
| `render/DrawStreamWriter.h` | DrawStreamWriter状态机 |
| `render/DrawStreamWriter.cpp` | 脏标记编码实现 |
| `render/DrawStreamDecoder.h` | DrawStreamDecoder接口 |
| `render/BindGroups.h` | BindGroupSetSlot定义，BindGroupDesc |
| `render/TransientAllocator.h` | TransientAllocator定义 |
| `rhi/RHICommandList.h` | CommandList抽象接口 |
| `common/HandlePool.h` | HandlePool模板实现 |
| `common/Handles.h` | Handle类型定义 |

---

## 附录B：调试技巧

### 启用Semaphore调试

```bash
cmake -DVK_SEMAPHORE_DEBUG=ON ..
```

### RenderDoc/PIX Profiling标记

```cpp
cmd.beginEvent("MyPass");
// ... 绘制命令
cmd.endEvent();
```

### DrawStream调试输出

```cpp
// 在DrawStreamWriter::draw()后添加
for (const auto& entry : writer.entries()) {
    printf("Entry type=%d\n", static_cast<int>(entry.type));
}
```

---

## 附录C：扩展检查清单

添加新Pass时，确保：

- [ ] Pass类继承正确的基类 (ComputePassNode 或 RenderPassNode)
- [ ] 实现getName(), getDependencies(), execute()
- [ ] 依赖声明正确 (资源类型, 访问模式, Shader Stage)
- [ ] 使用DrawStreamWriter录制绘制命令 (Graphics Pass)
- [ ] 在Renderer::init()中注册Pass
- [ ] Pass顺序符合依赖关系 (在PassExecutor中按顺序执行)
- [ ] 添加必要的Pseudo-Handle常量 (如kPassShadowMapHandle)
- [ ] 在Renderer::drawFrame()中绑定新资源到PassExecutor