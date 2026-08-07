#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>

#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHIFactory.h"
#include "../rhi/RHISurface.h"
#include "../rhi/RHISwapchain.h"
#include "../rhi/d3d12/D3D12Device.h"

#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>

#include <array>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

class GlfwScope
{
public:
  GlfwScope()
  {
    if(glfwInit() != GLFW_TRUE)
      throw std::runtime_error("glfwInit failed");
  }

  ~GlfwScope()
  {
    if(m_window != nullptr)
      glfwDestroyWindow(m_window);
    glfwTerminate();
  }

  GLFWwindow* createWindow()
  {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    m_window = glfwCreateWindow(320, 180, "D3D12 RHI integration", nullptr, nullptr);
    if(m_window == nullptr)
      throw std::runtime_error("glfwCreateWindow failed");
    return m_window;
  }

private:
  GLFWwindow* m_window{nullptr};
};

std::vector<std::string> collectValidationErrors(ID3D12InfoQueue* queue)
{
  std::vector<std::string> errors;
  const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
  for(UINT64 index = 0; index < count; ++index)
  {
    SIZE_T size = 0;
    if(FAILED(queue->GetMessage(index, nullptr, &size)) || size == 0)
      continue;

    std::vector<unsigned char> storage(size);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if(FAILED(queue->GetMessage(index, message, &size)))
      continue;
    if(message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
       message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
    {
      errors.emplace_back(message->pDescription != nullptr
                            ? message->pDescription
                            : "D3D12 validation error without description");
    }
  }
  return errors;
}

}  // namespace

int main()
{
  using namespace demo::rhi;

  try
  {
    GlfwScope glfw;
    GLFWwindow* window = glfw.createWindow();

    auto device = createDevice(BackendType::d3d12);
    DeviceCreateInfo createInfo{};
    createInfo.enableValidationLayers = true;
    device->init(createInfo);

    auto* d3dDevice = dynamic_cast<demo::rhi::d3d12::D3D12Device*>(device.get());
    if(d3dDevice == nullptr || !d3dDevice->isValidationEnabled())
      throw std::runtime_error("D3D12 validation layer was not enabled");

    ID3D12InfoQueue* infoQueue = nullptr;
    const HRESULT queueResult =
      static_cast<ID3D12Device*>(d3dDevice->getD3D12Device())->QueryInterface(
        IID_PPV_ARGS(&infoQueue));
    if(FAILED(queueResult) || infoQueue == nullptr)
      throw std::runtime_error("ID3D12InfoQueue is unavailable");
    infoQueue->ClearStoredMessages();

    auto surface = createSurface(BackendType::d3d12);
    device->initSurface(*surface, WindowHandle{window});
    auto swapchain = device->createSwapchain(*surface, false);
    swapchain->rebuild();
    if(swapchain->needsRebuild())
      throw std::runtime_error("D3D12 swapchain could not be built");

    Queue* graphicsQueue = device->getQueue(QueueClass::graphics);
    if(graphicsQueue == nullptr)
      throw std::runtime_error("D3D12 graphics queue is unavailable");
    auto allocator = device->createCommandAllocator(QueueClass::graphics);
    auto commandBuffer = allocator->createCommandBuffer();
    if(commandBuffer == nullptr)
      throw std::runtime_error("D3D12 command allocator returned no command buffer");

    const AcquireResult acquired = swapchain->acquireNextImage();
    if(acquired.status != AcquireResult::Status::success || acquired.texture.isNull())
      throw std::runtime_error("D3D12 swapchain image acquisition failed");

    commandBuffer->begin(*allocator);

    const TextureBarrier beginBarrier{
      .texture = acquired.texture,
      .before = ResourceState::Present,
      .after = ResourceState::ColorAttachment,
    };
    commandBuffer->resourceBarrier(std::span{&beginBarrier, 1}, {});

    const RenderTargetDesc target{
      .texture = acquired.texture,
      .view = swapchain->textureView(acquired.imageIndex),
      .state = ResourceState::ColorAttachment,
      .loadOp = LoadOp::clear,
      .storeOp = StoreOp::store,
      .clearColor = {0.125f, 0.25f, 0.5f, 1.0f},
    };
    const RenderPassDesc pass{
      .renderArea = {{0, 0}, swapchain->getExtent()},
      .colorTargets = std::span{&target, 1},
    };
    commandBuffer->beginEvent("DX12 validation clear");
    (void)commandBuffer->beginRenderPass(pass);
    commandBuffer->endEncoding();
    commandBuffer->endEvent();

    const TextureBarrier endBarrier{
      .texture = acquired.texture,
      .before = ResourceState::ColorAttachment,
      .after = ResourceState::Present,
    };
    commandBuffer->resourceBarrier(std::span{&endBarrier, 1}, {});
    commandBuffer->end();

    std::array<CommandBuffer*, 1> commandBuffers{commandBuffer.get()};
    std::array<SubmitWaitPoint, 1> waitPoints{acquired.waitPoint};
    std::array<SubmitSignalPoint, 1> signalPoints{acquired.signalPoint};
    const SubmissionToken token = graphicsQueue->submit(SubmitBatch{
      .commandBuffers = commandBuffers,
      .waitPoints = acquired.waitPoint.isValid()
        ? std::span<const SubmitWaitPoint>{waitPoints}
        : std::span<const SubmitWaitPoint>{},
      .signalPoints = acquired.signalPoint.isValid()
        ? std::span<const SubmitSignalPoint>{signalPoints}
        : std::span<const SubmitSignalPoint>{},
      .debugLabel = "D3D12 validation clear",
    });
    graphicsQueue->wait(token);
    const PresentResult present = swapchain->present();
    if(present.status != PresentResult::Status::success)
      throw std::runtime_error("D3D12 swapchain present failed");
    device->waitIdle();

    const std::vector<std::string> validationErrors = collectValidationErrors(infoQueue);
    if(!validationErrors.empty())
    {
      for(const std::string& error : validationErrors)
        std::cerr << "D3D12 validation: " << error << '\n';
      infoQueue->Release();
      return 1;
    }

    infoQueue->Release();
    swapchain.reset();
    surface.reset();
    device->deinit();

    std::cout << "D3D12 clear/present integration passed with a clean Debug Layer\n";
    return 0;
  }
  catch(const std::exception& error)
  {
    std::cerr << "D3D12 clear/present integration failed: " << error.what() << '\n';
    return 1;
  }
}
