#include "../rhi/RHIDevice.h"
#include "../rhi/RHIFactory.h"
#include "../rhi/RHISurface.h"

#include <cassert>
#include <stdexcept>
#include <string_view>

int main()
{
  using demo::rhi::BackendType;

  static_assert(demo::rhi::defaultBackend() == BackendType::vulkan);
  assert(std::string_view(demo::rhi::toString(BackendType::vulkan)) == "vulkan");
  assert(std::string_view(demo::rhi::toString(BackendType::d3d12)) == "d3d12");
  assert(std::string_view(demo::rhi::toString(BackendType::metal)) == "metal");
  assert(demo::rhi::isBackendAvailable(BackendType::vulkan));

  auto defaultDevice = demo::rhi::createDevice();
  auto defaultSurface = demo::rhi::createSurface();
  assert(defaultDevice != nullptr);
  assert(defaultSurface != nullptr);
  assert(defaultDevice->getBackendInfo().type == BackendType::vulkan);
  assert(std::string_view(defaultDevice->getBackendInfo().apiName) == "Vulkan");

#if defined(_WIN32) && defined(DEMO_HAS_D3D12)
  assert(demo::rhi::isBackendAvailable(BackendType::d3d12));
  auto d3d12Device = demo::rhi::createDevice(BackendType::d3d12);
  assert(d3d12Device != nullptr);
  assert(d3d12Device->getBackendInfo().type == BackendType::d3d12);
  assert(std::string_view(d3d12Device->getBackendInfo().apiName) == "Direct3D 12");
  assert(demo::rhi::createSurface(BackendType::d3d12) != nullptr);
#else
  assert(!demo::rhi::isBackendAvailable(BackendType::d3d12));
  bool rejectedUnavailableBackend = false;
  try
  {
    (void)demo::rhi::createDevice(BackendType::d3d12);
  }
  catch(const std::runtime_error&)
  {
    rejectedUnavailableBackend = true;
  }
  assert(rejectedUnavailableBackend);
#endif

  assert(!demo::rhi::isBackendAvailable(BackendType::metal));
  return 0;
}
