#include "../rhi/vulkan/VulkanBarrierConversions.h"
#include "../rhi/vulkan/VulkanCommandBuffer.h"
#include "../rhi/vulkan/VulkanDeviceLossPolicy.h"
#include "../rhi/vulkan/VulkanPipelineConversions.h"
#include "../rhi/vulkan/VulkanQueueConversions.h"
#include "../rhi/vulkan/VulkanResourceTable.h"
#include "../rhi/vulkan/VulkanShaderConversions.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace {

template <typename NativeHandle>
NativeHandle fakeNativeHandle(uintptr_t value)
{
  if constexpr(std::is_pointer_v<NativeHandle>)
    return reinterpret_cast<NativeHandle>(value);
  else
    return static_cast<NativeHandle>(value);
}

void testVulkanEncoderScopeInvalidation()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanResourceTable table;
  VulkanRenderEncoder render;
  render.prepare(fakeNativeHandle<VkCommandBuffer>(1), &table, nullptr);
  render.invalidate();
  bool renderRejected = false;
  try
  {
    render.setViewport(Viewport{});
  }
  catch(const std::runtime_error&)
  {
    renderRejected = true;
  }
  assert(renderRejected);

  VulkanComputeEncoder compute;
  compute.prepare(fakeNativeHandle<VkCommandBuffer>(2), &table, nullptr);
  compute.invalidate();
  bool computeRejected = false;
  try
  {
    compute.dispatch(DispatchDesc{});
  }
  catch(const std::runtime_error&)
  {
    computeRejected = true;
  }
  assert(computeRejected);
}

void testVulkanResourceLifetimeTracking()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanResourceTable table;
  const QueueIdentity graphics{1, 101};
  const QueueIdentity compute{2, 101};
  const SubmissionToken graphics4{graphics, 4};
  const SubmissionToken graphics9{graphics, 9};
  const SubmissionToken compute3{compute, 3};

  const BufferHandle first = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(11)},
    BufferColdRecord{
      .desc = BufferDesc{.size = 64},
      .debugName = "first",
    });
  table.markBufferSubmitted(first, graphics4);
  table.markBufferSubmitted(first, graphics9);
  table.markBufferSubmitted(first, compute3);
  const BufferRecord firstRemoved = table.removeBuffer(first);
  assert(firstRemoved.cold.desc.size == 64);
  assert(firstRemoved.cold.debugName == "first");
  assert(firstRemoved.hot.pendingUses.count == 2);
  const std::array completed{
    SubmissionToken{graphics, 9},
    SubmissionToken{compute, 3},
  };
  assert(firstRemoved.hot.pendingUses.isSatisfiedBy(completed));
  assert(table.tryGetBufferHot(first) == nullptr);

  const BufferHandle reused = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(12)},
    BufferColdRecord{
      .desc = BufferDesc{.size = 128},
      .debugName = "reused",
    });
  assert(reused.index == first.index);
  assert(reused.generation != first.generation);
  assert(table.tryGetBufferCold(reused)->desc.size == 128);
  assert(table.tryGetBufferCold(reused)->debugName == "reused");

  const TextureHandle texture = table.registerTexture(
    fakeNativeHandle<VkImage>(21),
    TextureColdRecord{
      .desc = TextureDesc{.extent = Extent3D{4, 4, 1}},
      .owned = false,
    });
  const TextureViewHandle view = table.registerTextureView(
    fakeNativeHandle<VkImageView>(22),
    TextureViewColdRecord{
      .parentTexture = texture,
      .owned = false,
    });
  table.markTextureViewSubmitted(view, graphics9);
  const TextureViewRecord viewRemoved = table.removeTextureView(view);
  const TextureRecord textureRemoved = table.removeTexture(texture);
  assert(viewRemoved.hot.pendingUses.count == 1);
  assert(textureRemoved.hot.pendingUses.count == 1);

  const BufferHandle tableBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(31)},
    BufferColdRecord{.desc = BufferDesc{.size = 32}});
  const SamplerHandle sampler =
    table.registerSampler(fakeNativeHandle<VkSampler>(32));
  const ArgumentTableHandle argumentTable = table.registerArgumentTable(
    fakeNativeHandle<VkDescriptorSet>(33), {}, ArgumentTableLifetime::persistent);
  table.recordArgumentTableResource(argumentTable, 0, 0, residencyResource(tableBuffer));
  table.recordArgumentTableResource(
    argumentTable, 1, 0,
    ResidencyResource{
      ResidencyResourceKind::sampler, sampler.index, sampler.generation});
  assert(table.validateArgumentTableForSubmit(argumentTable));
  table.markArgumentTableSubmitted(argumentTable, compute3);

  const ArgumentTableRecord argumentRemoved =
    table.removeArgumentTable(argumentTable);
  const BufferRecord tableBufferRemoved = table.removeBuffer(tableBuffer);
  const SamplerRecord samplerRemoved = table.removeSampler(sampler);
  assert(argumentRemoved.pendingUses.count == 1);
  assert(tableBufferRemoved.hot.pendingUses.count == 1);
  assert(samplerRemoved.pendingUses.count == 1);

  const BufferHandle staleBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(41)},
    BufferColdRecord{});
  const BufferHandle replacementBuffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(42)},
    BufferColdRecord{});
  const ArgumentTableHandle staleTable = table.registerArgumentTable(
    fakeNativeHandle<VkDescriptorSet>(43), {}, ArgumentTableLifetime::persistent);
  table.recordArgumentTableResource(staleTable, 0, 0, residencyResource(staleBuffer));
  assert(table.validateArgumentTableForSubmit(staleTable));
  table.recordArgumentTableResource(
    staleTable, 0, 0, residencyResource(replacementBuffer));
  (void)table.removeBuffer(staleBuffer);
  assert(table.validateArgumentTableForSubmit(staleTable));
  (void)table.removeBuffer(replacementBuffer);
  assert(!table.validateArgumentTableForSubmit(staleTable));
  (void)table.removeArgumentTable(staleTable);

  (void)table.removeBuffer(reused);
}

void testVulkanResidencySetValidation()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  VulkanResourceTable table;
  const BufferHandle buffer = table.registerBuffer(
    BufferHotRecord{.nativeBuffer = fakeNativeHandle<VkBuffer>(1)},
    BufferColdRecord{.desc = BufferDesc{.size = 64}});
  const TextureHandle texture = table.registerTexture(
    fakeNativeHandle<VkImage>(2), TextureColdRecord{.owned = false});

  const Result<ResidencySetHandle> invalidSet =
    table.registerResidencySet(ResidencySetDesc{});
  assert(!invalidSet && invalidSet.error.code == RHIErrorCode::invalidArgument);

  const Result<ResidencySetHandle> created = table.registerResidencySet(
    ResidencySetDesc{.maxResources = 1, .debugName = "Residency validation"});
  assert(created);
  const ResidencySetHandle set = created.value;
  const std::array addBuffer{residencyResource(buffer)};
  assert(table.updateResidencySet(
    set, ResidencyUpdateBatch{.add = addBuffer}));
  assert(table.updateResidencySet(
    set, ResidencyUpdateBatch{.add = addBuffer}));

  const std::array addTexture{residencyResource(texture)};
  const RHIResult capacity = table.updateResidencySet(
    set, ResidencyUpdateBatch{.add = addTexture});
  assert(!capacity && capacity.error.code == RHIErrorCode::invalidState);

  const std::array missingRemoval{residencyResource(texture)};
  const RHIResult missing = table.updateResidencySet(
    set, ResidencyUpdateBatch{.remove = missingRemoval});
  assert(!missing && missing.error.code == RHIErrorCode::invalidHandle);

  const std::array removeBuffer{residencyResource(buffer)};
  assert(table.updateResidencySet(
    set, ResidencyUpdateBatch{.remove = removeBuffer}));
  assert(table.removeResidencySet(set));
  const RHIResult stale = table.updateResidencySet(set, {});
  assert(!stale && stale.error.code == RHIErrorCode::invalidHandle);

  (void)table.removeBuffer(buffer);
  (void)table.removeTexture(texture);
}

}  // namespace

int main()
{
  using namespace demo::rhi;
  using namespace demo::rhi::vulkan;

  constexpr std::array portableFormats{
    TextureFormat::rgba8Unorm,
    TextureFormat::rgba8Srgb,
    TextureFormat::bgra8Unorm,
    TextureFormat::rgba16Sfloat,
    TextureFormat::d16Unorm,
    TextureFormat::d32Sfloat,
    TextureFormat::d24UnormS8,
    TextureFormat::d32SfloatS8,
    TextureFormat::rg16Sfloat,
    TextureFormat::r32Sfloat,
    TextureFormat::r16Sfloat,
    TextureFormat::rgba8Snorm,
    TextureFormat::r11g11b10Ufloat,
    TextureFormat::bc6hUfloatBlock,
    TextureFormat::bc6hSfloatBlock,
    TextureFormat::bc7UnormBlock,
    TextureFormat::bc7SrgbBlock,
  };
  for(TextureFormat format : portableFormats)
    assert(toNativeFormat(format) != VK_FORMAT_UNDEFINED);

  static_assert(toNativeFormat(TextureFormat::undefined) == VK_FORMAT_UNDEFINED);
  static_assert(toVkSampleCount(SampleCount::count1) == VK_SAMPLE_COUNT_1_BIT);
  static_assert(toVkSampleCount(SampleCount::count8) == VK_SAMPLE_COUNT_8_BIT);
  static_assert(toVkImageAspect(TextureAspect::depthStencil) ==
                (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
  static_assert(toVkDescriptorType(ArgumentType::uniformBuffer, true) ==
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
  static_assert(toVkShaderStage(ShaderStage::compute) == VK_SHADER_STAGE_COMPUTE_BIT);
  static_assert(toVkShaderStageFlags(ShaderStage::vertex | ShaderStage::fragment) ==
                (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT));
  static_assert(toVkPipelineStage2(StageFlags::none) ==
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
  static_assert(toVkSubmitStage(SubmitStage::transfer) ==
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
  static_assert(toVkTopology(PrimitiveTopology::triangleList) ==
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
  static_assert(toVkImageLayout(ResourceState::Present) ==
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  static_assert(permitsEmergencyRetirementDrain(VK_SUCCESS));
  static_assert(permitsEmergencyRetirementDrain(VK_ERROR_DEVICE_LOST));
  static_assert(!permitsEmergencyRetirementDrain(VK_ERROR_OUT_OF_HOST_MEMORY));

  constexpr VkMemoryBarrier2 barrier = makeMemoryBarrier2(
    StageFlags::transfer, StageFlags::compute,
    HazardFlags::bufferWrites | HazardFlags::storageBufferReadWrite |
      HazardFlags::drawArguments);
  static_assert(barrier.srcStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
  static_assert(barrier.srcAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT);
  static_assert(barrier.dstStageMask ==
                (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                 VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT));
  static_assert(barrier.dstAccessMask ==
                (VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                 VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT));
  testVulkanEncoderScopeInvalidation();
  testVulkanResourceLifetimeTracking();
  testVulkanResidencySetValidation();
  return 0;
}
