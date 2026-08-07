#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIFactory.h"
#include "../rhi/d3d12/D3D12Device.h"
#include "../rhi/d3d12/D3D12CommandBuffer.h"
#include "../render/UploadManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>

int main()
{
  using namespace demo::rhi;

  try
  {
    auto device = createDevice(BackendType::d3d12);
    DeviceCreateInfo createInfo{};
    createInfo.enableValidationLayers = true;
    device->init(createInfo);
    auto* d3d12Device = dynamic_cast<demo::rhi::d3d12::D3D12Device*>(device.get());
    if(d3d12Device == nullptr || !d3d12Device->isValidationEnabled())
    {
      std::cerr << "D3D12 validation layer was not enabled\n";
      return 1;
    }

    demo::rhi::d3d12::D3D12RenderEncoder expiredRender;
    expiredRender.prepare(nullptr, d3d12Device, nullptr);
    expiredRender.invalidate();
    bool rejectedExpiredRender = false;
    try
    {
      expiredRender.setViewport(Viewport{});
    }
    catch(const std::runtime_error&)
    {
      rejectedExpiredRender = true;
    }
    demo::rhi::d3d12::D3D12ComputeEncoder expiredCompute;
    expiredCompute.prepare(nullptr, d3d12Device, nullptr);
    expiredCompute.invalidate();
    bool rejectedExpiredCompute = false;
    try
    {
      expiredCompute.dispatch(DispatchDesc{});
    }
    catch(const std::runtime_error&)
    {
      rejectedExpiredCompute = true;
    }
    if(!rejectedExpiredRender || !rejectedExpiredCompute)
    {
      std::cerr << "D3D12 accepted an encoder call outside its encoding scope\n";
      return 1;
    }

    if(device->getQueue(QueueClass::graphics) == nullptr ||
       device->getQueue(QueueClass::compute) == nullptr ||
       device->getQueue(QueueClass::transfer) == nullptr ||
       !device->getQueue(QueueClass::graphics)->info().isValid() ||
       !device->getQueue(QueueClass::compute)->info().isValid() ||
       !device->getQueue(QueueClass::transfer)->info().isValid())
    {
      std::cerr << "D3D12 device did not expose all required queues\n";
      return 1;
    }

    const BufferDesc uploadDesc{
      .size = 256,
      .usage = BufferUsageFlags::transferSrc | BufferUsageFlags::shaderDeviceAddress,
      .memoryUsage = MemoryUsage::cpuToGpu,
      .allowGpuAddress = true,
      .debugName = "D3D12 smoke upload buffer",
    };
    const BufferHandle upload = device->createBuffer(uploadDesc);
    if(!upload.isValid() || !device->getBufferGpuAddress(upload).isValid())
    {
      std::cerr << "D3D12 upload buffer did not expose a valid handle and GPU address\n";
      return 1;
    }

    BufferHandle foreignUpload = upload;
    foreignUpload.generation ^= 0x00010000u;
    if(foreignUpload.generation == 0 || foreignUpload.generation == upload.generation)
      foreignUpload.generation = upload.generation + 1u;
    if(device->getBufferGpuAddress(foreignUpload).isValid() ||
       device->mapBuffer(foreignUpload) != nullptr ||
       d3d12Device->resolveBuffer(foreignUpload) != nullptr)
    {
      std::cerr << "D3D12 accepted a wrong-device buffer generation\n";
      return 1;
    }
    device->destroyBuffer(foreignUpload);
    if(!device->getBufferGpuAddress(upload).isValid())
    {
      std::cerr << "D3D12 wrong-device destroy invalidated the owned buffer\n";
      return 1;
    }

    auto* mapped = static_cast<uint32_t*>(device->mapBuffer(upload));
    if(mapped == nullptr)
    {
      std::cerr << "D3D12 upload buffer was not CPU mappable\n";
      return 1;
    }
    mapped[0] = 0x12345678u;
    mapped[63] = 0xabcdef01u;
    const Result<MappedBufferRange> explicitMap = device->mapBufferRange(
      upload, BufferMapDesc{.offset = sizeof(uint32_t),
                            .size = sizeof(uint32_t) * 2u,
                            .mode = BufferMapMode::write});
    if(!explicitMap || explicitMap.value.data == nullptr ||
       explicitMap.value.size != sizeof(uint32_t) * 2u ||
       !explicitMap.value.coherent)
    {
      std::cerr << "D3D12 explicit upload-buffer mapping contract failed\n";
      return 1;
    }
    device->unmapBuffer(upload);
    device->destroyBuffer(upload);
    if(device->getBufferGpuAddress(upload).isValid() || device->mapBuffer(upload) != nullptr)
    {
      std::cerr << "D3D12 destroyed buffer handle remained accessible\n";
      return 1;
    }
    const Result<MappedBufferRange> staleMap = device->mapBufferRange(
      upload, BufferMapDesc{.offset = 0, .size = sizeof(uint32_t),
                            .mode = BufferMapMode::read});
    if(staleMap || staleMap.error.code != RHIErrorCode::invalidHandle)
    {
      std::cerr << "D3D12 explicit map accepted a stale buffer handle\n";
      return 1;
    }

    const BufferDesc gpuOnlyDesc{
      .size = 256,
      .usage = BufferUsageFlags::storage,
      .memoryUsage = MemoryUsage::gpuOnly,
      .debugName = "D3D12 smoke GPU-only buffer",
    };
    const BufferHandle gpuOnly = device->createBuffer(gpuOnlyDesc);
    if(!gpuOnly.isValid() || device->mapBuffer(gpuOnly) != nullptr)
    {
      std::cerr << "D3D12 GPU-only buffer mapping behavior was invalid\n";
      return 1;
    }
    device->destroyBuffer(gpuOnly);


    demo::UploadManager uploadManager;
    uploadManager.init(*device, 2);
    if(uploadManager.transferExecutionQueueClass() != QueueClass::transfer)
    {
      std::cerr << "D3D12 UploadManager did not select the copy queue\n";
      return 1;
    }
    const BufferHandle transferDestination = device->createBuffer(BufferDesc{
      .size = 16,
      .usage = BufferUsageFlags::transferSrc | BufferUsageFlags::transferDst,
      .memoryUsage = MemoryUsage::gpuOnly,
      .debugName = "D3D12 transfer destination",
    });
    const BufferHandle transferReadback = device->createBuffer(BufferDesc{
      .size = 16,
      .usage = BufferUsageFlags::transferDst,
      .memoryUsage = MemoryUsage::gpuToCpu,
      .debugName = "D3D12 transfer readback",
    });
    const std::array<uint32_t, 4> transferSourceData{
      0x10203040u,
      0x50607080u,
      0x90a0b0c0u,
      0xd0e0f001u,
    };
    uploadManager.stageAndExecuteTransfer(
      std::as_bytes(std::span{transferSourceData}),
      [&](CommandBuffer& commands, const demo::UploadManager::StagingSlice& staging) {
        ComputeEncoder* copy = commands.beginComputePass();
        copy->copyBuffer(
          staging.buffer, staging.offset, transferDestination, 0, staging.size);
        commands.endEncoding();
      });
    uploadManager.executeTransfer([&](CommandBuffer& commands) {
      ComputeEncoder* copy = commands.beginComputePass();
      copy->copyBuffer(transferDestination, 0, transferReadback, 0, 16);
      commands.endEncoding();
    });
    uploadManager.flush(true);

    const auto* transferReadbackData =
      static_cast<const uint32_t*>(device->mapBuffer(transferReadback));
    if(transferReadbackData == nullptr ||
       transferReadbackData[0] != 0x10203040u ||
       transferReadbackData[1] != 0x50607080u ||
       transferReadbackData[2] != 0x90a0b0c0u ||
       transferReadbackData[3] != 0xd0e0f001u)
    {
      std::cerr << "D3D12 transfer queue round trip produced incorrect data\n";
      return 1;
    }
    device->unmapBuffer(transferReadback);
    uploadManager.shutdown();
    device->destroyBuffer(transferReadback);
    device->destroyBuffer(transferDestination);
    const Result<ResidencySetHandle> residency =
      device->createResidencySet(ResidencySetDesc{});
    if(residency || residency.error.code != RHIErrorCode::unsupported)
    {
      std::cerr << "D3D12 residency capability gate did not report unsupported\n";
      return 1;
    }

    const ArgumentBinding tableBinding{
      .binding = 0,
      .type = ArgumentType::uniformBuffer,
      .visibility = ShaderStage::all,
      .arrayCount = 1,
    };
    const ArgumentLayoutHandle tableLayout = device->createArgumentLayout(
      ArgumentLayoutDesc{.bindings = std::span{&tableBinding, 1},
                         .debugName = "D3D12 smoke argument layout"});
    const ArgumentTableHandle table = device->createArgumentTable(
      ArgumentTableCreateDesc{.layout = tableLayout,
                              .lifetime = ArgumentTableLifetime::persistent,
                              .debugName = "D3D12 smoke persistent table"});
    BufferHandle tableBuffer = device->createBuffer(BufferDesc{
      .size = 256,
      .usage = BufferUsageFlags::uniform,
      .memoryUsage = MemoryUsage::cpuToGpu,
      .debugName = "D3D12 smoke argument buffer",
    });
    ArgumentWrite tableWrite{
      .binding = 0,
      .type = ArgumentType::uniformBuffer,
      .buffer = tableBuffer,
      .size = 256,
    };
    const std::span<const ArgumentWrite> tableWrites(&tableWrite, 1);
    device->updateArgumentTable(table, tableWrites);
    const BufferHandle replacementTableBuffer = device->createBuffer(BufferDesc{
      .size = 256,
      .usage = BufferUsageFlags::uniform,
      .memoryUsage = MemoryUsage::cpuToGpu,
      .debugName = "D3D12 replacement argument buffer",
    });
    tableWrite.buffer = replacementTableBuffer;
    device->updateArgumentTable(table, tableWrites);
    device->destroyBuffer(tableBuffer);
    if(!d3d12Device->validateArgumentTableForSubmit(table))
    {
      std::cerr << "D3D12 argument table retained a replaced resource reference\n";
      return 1;
    }
    tableBuffer = replacementTableBuffer;

    Queue* graphicsQueue = device->getQueue(QueueClass::graphics);

    auto transferAllocator = device->createCommandAllocator(QueueClass::transfer);
    auto transferCommands = transferAllocator->createCommandBuffer();
    transferCommands->begin(*transferAllocator);
    bool rejectedTransferRender = false;
    try
    {
      (void)transferCommands->beginRenderPass(RenderPassDesc{});
    }
    catch(const std::runtime_error&)
    {
      rejectedTransferRender = true;
    }
    if(!rejectedTransferRender)
    {
      std::cerr << "D3D12 transfer queue accepted a render pass\n";
      return 1;
    }
    ComputeEncoder* transferEncoder = transferCommands->beginComputePass();
    bool rejectedTransferDispatch = false;
    try
    {
      transferEncoder->dispatch(DispatchDesc{});
    }
    catch(const std::runtime_error&)
    {
      rejectedTransferDispatch = true;
    }
    transferCommands->endEncoding();
    transferCommands->end();
    if(!rejectedTransferDispatch)
    {
      std::cerr << "D3D12 transfer queue accepted a compute dispatch\n";
      return 1;
    }

    auto commandAllocator = device->createCommandAllocator(QueueClass::graphics);
    const ArgumentTableHandle staleSubmitTable = device->createArgumentTable(
      ArgumentTableCreateDesc{.layout = tableLayout,
                              .lifetime = ArgumentTableLifetime::persistent,
                              .debugName = "D3D12 stale-submit table"});
    auto staleCommands = commandAllocator->createCommandBuffer();
    staleCommands->begin(*commandAllocator);
    staleCommands->end();
    auto* staleD3D12Commands =
      dynamic_cast<demo::rhi::d3d12::D3D12CommandBuffer*>(staleCommands.get());
    if(staleD3D12Commands == nullptr)
      throw std::runtime_error("D3D12 command buffer type mismatch");
    staleD3D12Commands->trackArgumentTable(staleSubmitTable);
    device->destroyArgumentTable(staleSubmitTable);

    CommandBuffer* staleSubmitCommand = staleCommands.get();
    const SubmissionToken beforeRejectedSubmit = graphicsQueue->lastSubmittedToken();
    bool rejectedStaleTableSubmit = false;
    try
    {
      (void)graphicsQueue->submit(SubmitBatch{
        .commandBuffers = std::span<CommandBuffer* const>{&staleSubmitCommand, 1},
        .debugLabel = "D3D12 stale-table preflight",
      });
    }
    catch(const std::runtime_error&)
    {
      rejectedStaleTableSubmit = true;
    }
    if(!rejectedStaleTableSubmit ||
       graphicsQueue->lastSubmittedToken() != beforeRejectedSubmit)
    {
      std::cerr << "D3D12 stale argument table was not rejected before queue submission\n";
      return 1;
    }

    const ArgumentTableHandle staleResourceTable = device->createArgumentTable(
      ArgumentTableCreateDesc{
        .layout = tableLayout,
        .lifetime = ArgumentTableLifetime::persistent,
        .debugName = "D3D12 stale-resource table",
      });
    const BufferHandle staleResourceBuffer = device->createBuffer(BufferDesc{
      .size = 256,
      .usage = BufferUsageFlags::uniform,
      .memoryUsage = MemoryUsage::cpuToGpu,
      .debugName = "D3D12 stale-resource buffer",
    });
    const ArgumentWrite staleResourceWrite{
      .binding = 0,
      .type = ArgumentType::uniformBuffer,
      .buffer = staleResourceBuffer,
      .size = 256,
    };
    device->updateArgumentTable(
      staleResourceTable,
      std::span<const ArgumentWrite>{&staleResourceWrite, 1});
    auto staleResourceCommands = commandAllocator->createCommandBuffer();
    staleResourceCommands->begin(*commandAllocator);
    staleResourceCommands->end();
    auto* staleResourceD3D12 =
      dynamic_cast<demo::rhi::d3d12::D3D12CommandBuffer*>(
        staleResourceCommands.get());
    if(staleResourceD3D12 == nullptr)
      throw std::runtime_error("D3D12 command buffer type mismatch");
    staleResourceD3D12->trackArgumentTable(staleResourceTable);
    device->destroyBuffer(staleResourceBuffer);
    CommandBuffer* staleResourceCommand = staleResourceCommands.get();
    const SubmissionToken beforeStaleResourceSubmit =
      graphicsQueue->lastSubmittedToken();
    bool rejectedStaleResourceSubmit = false;
    try
    {
      (void)graphicsQueue->submit(SubmitBatch{
        .commandBuffers =
          std::span<CommandBuffer* const>{&staleResourceCommand, 1},
        .debugLabel = "D3D12 stale-resource preflight",
      });
    }
    catch(const std::runtime_error&)
    {
      rejectedStaleResourceSubmit = true;
    }
    if(!rejectedStaleResourceSubmit ||
       graphicsQueue->lastSubmittedToken() != beforeStaleResourceSubmit)
    {
      std::cerr << "D3D12 stale table resource was not rejected before queue submission\n";
      return 1;
    }
    device->destroyArgumentTable(staleResourceTable);

    auto commandBuffer = commandAllocator->createCommandBuffer();
    commandBuffer->begin(*commandAllocator);
    commandBuffer->end();
    CommandBuffer* submittedCommandBuffer = commandBuffer.get();
    const std::span<CommandBuffer* const> submittedBuffers(&submittedCommandBuffer, 1);
    const SubmissionToken completedUse = graphicsQueue->submit(
      SubmitBatch{.commandBuffers = submittedBuffers,
                  .debugLabel = "D3D12 argument-table completion smoke"});
    graphicsQueue->wait(completedUse);
    SubmissionToken foreignToken = completedUse;
    foreignToken.queue.generation ^= 0x00010000u;
    bool rejectedForeignToken = false;
    try
    {
      (void)graphicsQueue->isComplete(foreignToken);
    }
    catch(const std::runtime_error&)
    {
      rejectedForeignToken = true;
    }
    if(!rejectedForeignToken)
    {
      std::cerr << "D3D12 queue accepted a wrong-device submission token\n";
      return 1;
    }
    d3d12Device->markArgumentTableSubmitted(table, completedUse);
    device->updateArgumentTable(table, tableWrites);

    const SubmissionToken pendingUse{
      graphicsQueue->identity(), completedUse.value + 1000000u};
    d3d12Device->markArgumentTableSubmitted(table, pendingUse);
    bool rejectedPendingTableUpdate = false;
    try
    {
      device->updateArgumentTable(table, tableWrites);
    }
    catch(const std::runtime_error&)
    {
      rejectedPendingTableUpdate = true;
    }
    if(!rejectedPendingTableUpdate)
    {
      std::cerr << "D3D12 accepted an argument-table update with pending GPU work\n";
      return 1;
    }

    device->destroyArgumentTable(table);
    if(device->getArgumentTableLayout(table).isValid())
    {
      std::cerr << "D3D12 destroyed argument-table handle remained accessible\n";
      return 1;
    }
    bool rejectedDestroyedTableUpdate = false;
    try
    {
      device->updateArgumentTable(table, tableWrites);
    }
    catch(const std::runtime_error&)
    {
      rejectedDestroyedTableUpdate = true;
    }
    if(!rejectedDestroyedTableUpdate)
    {
      std::cerr << "D3D12 accepted an update through a destroyed argument-table handle\n";
      return 1;
    }
    device->destroyArgumentLayout(tableLayout);
    device->destroyBuffer(tableBuffer);
    commandAllocator->reset(completedUse);

    const TextureDesc textureDesc{
      .dimension = TextureDimension::e2D,
      .format = TextureFormat::rgba8Unorm,
      .usage = TextureUsageFlags::sampled | TextureUsageFlags::storage |
               TextureUsageFlags::transferDst,
      .extent = {.width = 64, .height = 64, .depth = 1},
      .mipLevels = 4,
      .arrayLayers = 1,
      .sampleCount = SampleCount::count1,
      .memoryUsage = MemoryUsage::gpuOnly,
      .debugName = "D3D12 smoke owned texture",
    };
    const TextureHandle texture = device->createTexture(textureDesc);
    if(!texture.isValid())
    {
      std::cerr << "D3D12 owned texture did not expose a valid handle\n";
      return 1;
    }

    const TextureViewCreateDesc viewDesc{
      .image = texture,
      .format = TextureFormat::rgba8Unorm,
      .viewType = ImageViewType::e2D,
      .aspect = TextureAspect::color,
      .baseMipLevel = 1,
      .levelCount = 3,
      .baseArrayLayer = 0,
      .layerCount = 1,
      .debugName = "D3D12 smoke owned texture view",
    };
    const TextureViewHandle view = device->createTextureView(viewDesc);
    if(!view.isValid())
    {
      std::cerr << "D3D12 owned texture view did not expose a valid handle\n";
      return 1;
    }

    device->destroyTexture(texture);
    bool rejectedStaleTexture = false;
    try
    {
      (void)device->createTextureView(viewDesc);
    }
    catch(const std::runtime_error&)
    {
      rejectedStaleTexture = true;
    }
    if(!rejectedStaleTexture)
    {
      std::cerr << "D3D12 destroyed texture handle remained usable\n";
      return 1;
    }
    device->destroyTextureView(view);
    device->destroyTextureView(view);
    device->destroyTexture(texture);

    TextureDesc unsupportedTexture = textureDesc;
    unsupportedTexture.memoryUsage = MemoryUsage::cpuToGpu;
    bool rejectedUnsupportedTexture = false;
    try
    {
      (void)device->createTexture(unsupportedTexture);
    }
    catch(const std::runtime_error&)
    {
      rejectedUnsupportedTexture = true;
    }
    if(!rejectedUnsupportedTexture)
    {
      std::cerr << "D3D12 accepted an unsupported CPU-visible texture\n";
      return 1;
    }

    device->waitIdle();
    std::cout << "D3D12 adapter: " << device->getDeviceName() << '\n';
    device->deinit();
    return 0;
  }
  catch(const std::exception& error)
  {
    const std::string_view message(error.what());
    if(message.starts_with("No hardware adapter") ||
       message.starts_with("D3D12 capability requirement failed"))
    {
      std::cout << "D3D12 smoke test skipped: " << message << '\n';
      return 77;
    }

    std::cerr << "D3D12 smoke test failed: " << message << '\n';
    return 1;
  }
}
