#include "../common/HandlePool.h"
#include "../rhi/RHIArgumentTable.h"
#include "../rhi/RHIBackend.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHICapabilities.h"
#include "../rhi/RHIDebugCounters.h"
#include "../rhi/RHIDebugValidation.h"
#include "../rhi/RHIResidency.h"
#include "../rhi/RHIResult.h"
#include "../rhi/RHIPipeline.h"
#include "../rhi/RHIResourceLifetime.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

namespace {

void testHandlePoolGenerationSafety()
{
  demo::HandlePool<demo::rhi::BufferHandle, uint32_t> pool;
  const demo::rhi::BufferHandle first = pool.emplace(17u);
  assert(first.isValid());
  assert(pool.liveCount() == 1);
  assert(pool.tryGet(first) != nullptr && *pool.tryGet(first) == 17u);

  assert(pool.destroy(first));
  assert(!pool.destroy(first));
  assert(pool.tryGet(first) == nullptr);

  const demo::rhi::BufferHandle reused = pool.emplace(23u);
  assert(reused.index == first.index);
  assert(reused.generation != first.generation);
  assert(pool.tryGet(first) == nullptr);
  assert(pool.tryGet(reused) != nullptr && *pool.tryGet(reused) == 23u);

  assert(demo::rhi::nextHandleGeneration(
           (std::numeric_limits<uint32_t>::max)()) == 1u);
  demo::HandlePool<demo::rhi::BufferHandle, uint32_t> otherPool;
  const demo::rhi::BufferHandle foreign = otherPool.emplace(99u);
  assert(foreign.generation != reused.generation);
  assert(pool.tryGet(foreign) == nullptr);
  assert(otherPool.tryGet(reused) == nullptr);
}

void testPipelineBindingSchema()
{
  using namespace demo::rhi;
  const std::array layouts{
    ArgumentLayoutHandle{1, 1},
    ArgumentLayoutHandle{2, 1},
  };
  const std::array roots{
    RootBindingDesc{
      .slot = 5,
      .kind = RootBindingKind::constants,
      .visibility = ShaderStage::vertex,
      .size = 16,
      .alignment = 16,
    },
    RootBindingDesc{
      .slot = 9,
      .kind = RootBindingKind::gpuPointer,
      .visibility = ShaderStage::compute,
      .size = kGpuPointerRootBindingSize,
      .alignment = 8,
    },
  };
  const PipelineBindingSchemaStorage storage{layouts, roots};
  const PipelineBindingSchemaDesc schema = storage.view();
  assert(validatePipelineBindingSchema(schema).valid());
  assert(schema.argumentSlots.size() == 2);
  assert(schema.argumentSlots[0].slot == 0);
  assert(schema.argumentSlots[1].slot == 1);
  assert(schema.rootBindings[0].slot == 5);
  assert(schema.rootBindings[1].slot == 9);

  std::array duplicateRoots{roots[0], roots[0]};
  const PipelineBindingSchemaDesc duplicateSchema{
    .rootBindings = duplicateRoots,
  };
  assert(validatePipelineBindingSchema(duplicateSchema).error ==
         PipelineBindingSchemaValidationError::duplicateRootSlot);

  RootBindingDesc invalidAlignment = roots[0];
  invalidAlignment.alignment = 3;
  const PipelineBindingSchemaDesc invalidAlignmentSchema{
    .rootBindings = std::span{&invalidAlignment, 1},
  };
  assert(validatePipelineBindingSchema(invalidAlignmentSchema).error ==
         PipelineBindingSchemaValidationError::invalidRootBindingAlignment);
}

void testQueueOperationCapabilities()
{
  using namespace demo::rhi;
  static_assert(supportsQueueOperation(QueueClass::graphics, QueueOperation::render));
  static_assert(supportsQueueOperation(QueueClass::graphics, QueueOperation::compute));
  static_assert(supportsQueueOperation(QueueClass::graphics, QueueOperation::transfer));
  static_assert(!supportsQueueOperation(QueueClass::compute, QueueOperation::render));
  static_assert(supportsQueueOperation(QueueClass::compute, QueueOperation::compute));
  static_assert(supportsQueueOperation(QueueClass::compute, QueueOperation::transfer));
  static_assert(!supportsQueueOperation(QueueClass::transfer, QueueOperation::render));
  static_assert(!supportsQueueOperation(QueueClass::transfer, QueueOperation::compute));
  static_assert(supportsQueueOperation(QueueClass::transfer, QueueOperation::transfer));
}

void testCommandBufferStateContract()
{
  using demo::rhi::CommandBufferState;
  using demo::rhi::canTransitionCommandBufferState;

  assert(canTransitionCommandBufferState(
    CommandBufferState::idle, CommandBufferState::recording));
  assert(canTransitionCommandBufferState(
    CommandBufferState::recording, CommandBufferState::executable));
  assert(canTransitionCommandBufferState(
    CommandBufferState::executable, CommandBufferState::submitted));
  assert(canTransitionCommandBufferState(
    CommandBufferState::submitted, CommandBufferState::reusable));
  assert(canTransitionCommandBufferState(
    CommandBufferState::reusable, CommandBufferState::recording));

  assert(!canTransitionCommandBufferState(
    CommandBufferState::idle, CommandBufferState::submitted));
  assert(!canTransitionCommandBufferState(
    CommandBufferState::recording, CommandBufferState::submitted));
  assert(!canTransitionCommandBufferState(
    CommandBufferState::submitted, CommandBufferState::recording));
}

void testArgumentTableUpdateContract()
{
  using demo::rhi::ArgumentTableUseState;
  using demo::rhi::canUpdateArgumentTable;

  assert(canUpdateArgumentTable(ArgumentTableUseState::idle, false));
  assert(canUpdateArgumentTable(
    ArgumentTableUseState::executableUnsubmitted, false));
  assert(!canUpdateArgumentTable(
    ArgumentTableUseState::pendingGpuWork, false));
  assert(canUpdateArgumentTable(
    ArgumentTableUseState::pendingGpuWork, true));
  assert(!canUpdateArgumentTable(
    ArgumentTableUseState::idle, true, true));
}

void testDeferredRetirement()
{
  using namespace demo::rhi;
  InlineDeferredDestructionQueue queue;
  const ResourceHandle first{ResourceKind::Buffer, 1, 1};
  const ResourceHandle second{ResourceKind::Texture, 2, 1};
  const QueueIdentity graphics{1, 1};
  const QueueIdentity compute{2, 1};

  SubmissionTokenSet firstDependencies{};
  assert(firstDependencies.record(SubmissionToken{graphics, 4}));
  assert(firstDependencies.record(SubmissionToken{graphics, 3}));
  SubmissionTokenSet secondDependencies{};
  assert(secondDependencies.record(SubmissionToken{graphics, 7}));
  assert(secondDependencies.record(SubmissionToken{compute, 2}));
  queue.enqueue({first, firstDependencies});
  queue.enqueue({second, secondDependencies});

  const std::array incomplete{
    SubmissionToken{graphics, 3},
    SubmissionToken{compute, 2},
  };
  assert(queue.process(incomplete) == 0);

  const std::array graphicsComplete{
    SubmissionToken{graphics, 4},
    SubmissionToken{compute, 2},
  };
  assert(queue.process(graphicsComplete) == 1);
  assert(queue.drainedResources().front() == first);
  assert(queue.pendingRetirements().size() == 1);

  const std::array computeIncomplete{
    SubmissionToken{graphics, 7},
    SubmissionToken{compute, 1},
  };
  assert(queue.process(computeIncomplete) == 0);

  const std::array allComplete{
    SubmissionToken{graphics, 7},
    SubmissionToken{compute, 2},
  };
  assert(queue.process(allComplete) == 1);
  assert(queue.drainedResources().front() == second);
  assert(queue.empty());

  std::array<SubmissionToken, SubmissionTokenSet::capacity + 1> uniqueTokens{
    SubmissionToken{QueueIdentity{10, 1}, 1},
    SubmissionToken{QueueIdentity{11, 1}, 1},
    SubmissionToken{QueueIdentity{12, 1}, 1},
    SubmissionToken{QueueIdentity{13, 1}, 1},
  };
  SubmissionTokenSet bounded{};
  assert(bounded.record(uniqueTokens[0]));
  assert(bounded.record(uniqueTokens[1]));
  assert(bounded.record(uniqueTokens[2]));
  assert(!bounded.record(uniqueTokens[3]));
}

void testResultAndResidencyContract()
{
  using namespace demo::rhi;
  constexpr auto unsupported = Result<ResidencySetHandle>::fail(
    RHIErrorCode::unsupported, "not supported");
  static_assert(!unsupported.succeeded());
  static_assert(unsupported.error.code == RHIErrorCode::unsupported);

  CapabilityReport report{};
  assert(!supportsTier(report, CapabilityTier::Residency));
  report.explicitResidency = ExplicitResidencyLevel::validatedNoOp;
  assert(supportsTier(report, CapabilityTier::Residency));
  CapabilityRequirements requirements{};
  requirements.requireCoreGraphics = false;
  requirements.requireCoreCompute = false;
  requirements.requireCoreBindless = false;
  requirements.requireResidency = true;
  assert(evaluateCapabilityRequirements(report, requirements) == RHICapabilityError::None);

  constexpr BufferHandle buffer{7, 3};
  constexpr ResidencyResource resource = residencyResource(buffer);
  static_assert(resource.kind == ResidencyResourceKind::buffer);
  static_assert(resource.index == buffer.index && resource.generation == buffer.generation);

  constexpr BufferMapDesc map{
    .offset = 16,
    .size = 32,
    .mode = BufferMapMode::readWrite,
  };
  static_assert(map.offset == 16 && map.size == 32);
}

void testBarrierStateValidation()
{
  using namespace demo::rhi;
  DebugResourceStateTracker tracker;

  const BufferBarrier computeWrite{
    .buffer = BufferHandle{1, 1},
    .before = ResourceState::General,
    .after = ResourceState::ShaderWrite,
  };
  assert(tracker.transition(computeWrite).valid());
  const BufferBarrier indirectRead{
    .buffer = computeWrite.buffer,
    .before = ResourceState::ShaderWrite,
    .after = ResourceState::IndirectArgument,
  };
  assert(tracker.transition(indirectRead).valid());

  const TextureBarrier transferWrite{
    .texture = TextureHandle{2, 1},
    .before = ResourceState::Undefined,
    .after = ResourceState::TransferDst,
  };
  assert(tracker.transition(transferWrite).valid());
  TextureBarrier sampled = transferWrite;
  sampled.before = ResourceState::TransferDst;
  sampled.after = ResourceState::ShaderRead;
  assert(tracker.transition(sampled).valid());

  const TextureBarrier renderTarget{
    .texture = TextureHandle{3, 1},
    .before = ResourceState::Undefined,
    .after = ResourceState::ColorAttachment,
  };
  assert(tracker.transition(renderTarget).valid());
  TextureBarrier renderTargetSampled = renderTarget;
  renderTargetSampled.before = ResourceState::ColorAttachment;
  renderTargetSampled.after = ResourceState::ShaderRead;
  assert(tracker.transition(renderTargetSampled).valid());

  const TextureBarrier presentSource{
    .texture = TextureHandle{4, 1},
    .before = ResourceState::ColorAttachment,
    .after = ResourceState::Present,
  };
  assert(tracker.transition(presentSource).valid());
  TextureBarrier mismatch = presentSource;
  mismatch.before = ResourceState::TransferDst;
  mismatch.after = ResourceState::Present;
  assert(tracker.transition(mismatch).error == ResourceStateValidationError::stateMismatch);

  constexpr AliasingResource empty{};
  constexpr AliasingResource buffer =
    AliasingResource::fromBuffer(BufferHandle{9, 2});
  constexpr AliasingResource texture =
    AliasingResource::fromTexture(TextureHandle{10, 3});
  static_assert(empty.isValid());
  static_assert(buffer.isValid() && buffer.kind == AliasingResourceKind::buffer);
  static_assert(texture.isValid() && texture.kind == AliasingResourceKind::texture);
  constexpr AliasingBarrier aliasing{.before = buffer, .after = texture};
  static_assert(aliasing.before.buffer.index == 9);
  static_assert(aliasing.after.texture.index == 10);
}

void testHotPathCounters()
{
  using namespace demo::rhi;
  resetHotPathCounters(BackendType::vulkan);
  incrementHotPathCounter(BackendType::vulkan, HotPathCounter::commandBufferBegins, 2);
  incrementHotPathCounter(BackendType::vulkan, HotPathCounter::queueSubmits);
  RHIHotPathCounters counters = snapshotHotPathCounters(BackendType::vulkan);
  assert(counters.commandBufferBegins == 2);
  assert(counters.queueSubmits == 1);
  assert(counters.stableRecordingBudgetMet());
  incrementHotPathCounter(
    BackendType::vulkan, HotPathCounter::commandRecordingHeapAllocations);
  counters = snapshotHotPathCounters(BackendType::vulkan);
  assert(!counters.stableRecordingBudgetMet());
  resetHotPathCounters(BackendType::vulkan);
}
void testBackendIdentityShape()
{
  using namespace demo::rhi;
  constexpr BackendInfo info{
    .type = BackendType::d3d12,
    .apiName = "Direct3D 12",
    .version = BackendVersion{.major = 12, .minor = 2, .nativeValue = 0xc200},
  };
  static_assert(info.type == BackendType::d3d12);
  static_assert(info.version.major == 12);
  static_assert(info.version.nativeValue == 0xc200);
}

}  // namespace

int main()
{
  testHandlePoolGenerationSafety();
  testPipelineBindingSchema();
  testQueueOperationCapabilities();
  testCommandBufferStateContract();
  testArgumentTableUpdateContract();
  testDeferredRetirement();
  testResultAndResidencyContract();
  testBarrierStateValidation();
  testHotPathCounters();
  testBackendIdentityShape();
  return 0;
}