#include "../render/ImGuiRhiTypes.h"
#include "../rhi/RHIEncoder.h"

#include <cassert>
#include <cstdint>

namespace
{
  void testTextureIdRoundTrip()
  {
    const demo::ui::TextureHandle handle{37u, 9u};
    const uint64_t encoded = demo::ui::encodeTextureId(handle);
    const demo::ui::TextureHandle decoded = demo::ui::decodeTextureId(encoded);

    assert(encoded != 0u);
    assert(decoded.index == handle.index);
    assert(decoded.generation == handle.generation);
    assert(demo::ui::decodeTextureId(0u).isNull());
  }

  void testProjectionUsesDisplayOrigin()
  {
    const demo::ui::Projection projection = demo::ui::makeProjection(
      {100.0f, 50.0f}, {800.0f, 600.0f});

    assert(projection.scaleX == 2.0f / 800.0f);
    assert(projection.scaleY == 2.0f / 600.0f);
    assert(projection.translateX == -1.0f - 100.0f * projection.scaleX);
    assert(projection.translateY == -1.0f - 50.0f * projection.scaleY);
  }

  void testScissorTransformsAndClamps()
  {
    const auto scissor = demo::ui::makeScissor(
      {-20.0f, 10.0f, 90.0f, 80.0f},
      {10.0f, 5.0f},
      {2.0f, 2.0f},
      128u,
      96u);

    assert(scissor.has_value());
    assert(scissor->offset.x == 0);
    assert(scissor->offset.y == 10);
    assert(scissor->extent.width == 128u);
    assert(scissor->extent.height == 86u);
  }

  void testScissorRejectsEmptyRect()
  {
    const auto scissor = demo::ui::makeScissor(
      {20.0f, 20.0f, 10.0f, 30.0f},
      {0.0f, 0.0f},
      {1.0f, 1.0f},
      128u,
      96u);

    assert(!scissor.has_value());
  }

  void testBufferGrowthIsMonotonic()
  {
    assert(demo::ui::nextBufferCapacity(0u, 100u) >= 100u);
    assert(demo::ui::nextBufferCapacity(256u, 257u) >= 257u);
    assert(demo::ui::nextBufferCapacity(256u, 200u) == 256u);
  }

  void testTextureCopyCarriesDestinationOffset()
  {
    const demo::rhi::BufferTextureCopyDesc copy{
      .textureOffset = {4, 8, 0},
      .width = 16u,
      .height = 32u,
    };

    assert(copy.textureOffset.x == 4);
    assert(copy.textureOffset.y == 8);
  }
}

int main()
{
  static_assert(demo::rhi::VertexFormat::r8g8b8a8Unorm != demo::rhi::VertexFormat::undefined);

  testTextureIdRoundTrip();
  testProjectionUsesDisplayOrigin();
  testScissorTransformsAndClamps();
  testScissorRejectsEmptyRect();
  testBufferGrowthIsMonotonic();
  testTextureCopyCarriesDestinationOffset();
  return 0;
}
