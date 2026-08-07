#include "../rhi/metal/Metal4Availability.h"
#include "../rhi/metal/Metal4Contract.h"

#include <cassert>
#include <cstdio>

int main()
{
  using namespace demo::rhi;
  using namespace demo::rhi::metal;

  constexpr Metal4ArgumentTableBinding binding{
    .table = ArgumentTableHandle{7, 2},
    .stages = ShaderStage::vertex | ShaderStage::fragment,
    .slot = 3,
  };
  static_assert(binding.table.isValid());
  static_assert(binding.slot == 3);
  static_assert(static_cast<uint32_t>(binding.stages) == 3u);
  constexpr Metal4GpuAddress address{0x1020304050607080ull};
  static_assert(address.toRhi().value == address.value);

  const Metal4Availability availability = queryMetal4Availability();
  const Metal4NativeContractStatus contract = queryMetal4NativeContract();
  assert(availability.sdkHasCoreApi == contract.sdkTypesCompiled);
  assert(availability.runtimeHasCoreApi == contract.runtimeTypesAvailable);
  assert(availability.deviceAvailable == contract.deviceAvailable);
  std::printf("Metal4 SDK=%d runtime=%d device=%d\n",
              contract.sdkTypesCompiled,
              contract.runtimeTypesAvailable,
              contract.deviceAvailable);
  return 0;
}
