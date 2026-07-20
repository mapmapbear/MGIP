#include "../loader/GltfLoader.h"
#include "../loader/SceneCacheSerializer.h"
#include "../render/SceneViewResolver.h"
#include "../scene/SceneAssetBuilder.h"
#include "../scene/SceneAssetSerializer.h"
#include "../scene/SceneAssetView.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const char* message)
{
  if(!condition) {
    throw std::runtime_error(message);
  }
}

bool nearlyEqual(float lhs, float rhs, float epsilon = 1.0e-4f)
{
  return std::abs(lhs - rhs) <= epsilon;
}

bool matricesNearlyEqual(const glm::mat4& lhs, const glm::mat4& rhs, float epsilon = 1.0e-4f)
{
  for(int column = 0; column < 4; ++column) {
    for(int row = 0; row < 4; ++row) {
      if(!nearlyEqual(lhs[column][row], rhs[column][row], epsilon)) {
        return false;
      }
    }
  }
  return true;
}

struct TemporaryDirectory
{
  std::filesystem::path path;

  TemporaryDirectory()
  {
    const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("mgif_scene_defaults_" + std::to_string(suffix));
    std::filesystem::create_directories(path);
  }

  ~TemporaryDirectory()
  {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

void writeTestScene(const std::filesystem::path& path)
{
  static constexpr const char* sceneJson = R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1, 2]}],
  "nodes": [
    {"name": "MainCameraNode", "camera": 0, "translation": [1.0, 2.0, 3.0]},
    {"name": "SunNode", "extensions": {"KHR_lights_punctual": {"light": 0}}},
    {"name": "PointNode", "translation": [4.0, 5.0, 6.0],
     "extensions": {"KHR_lights_punctual": {"light": 1}}}
  ],
  "cameras": [{
    "name": "MainCamera",
    "type": "perspective",
    "perspective": {"aspectRatio": 1.5, "yfov": 0.7, "znear": 0.2, "zfar": 500.0}
  }],
  "extensionsUsed": ["KHR_lights_punctual"],
  "extensions": {
    "KHR_lights_punctual": {
      "lights": [
        {"name": "Sun", "type": "directional", "color": [0.5, 0.75, 1.0], "intensity": 2.0},
        {"name": "Fill", "type": "point", "color": [1.0, 0.25, 0.1], "intensity": 4.0, "range": 12.0}
      ]
    }
  }
})json";

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << sceneJson;
  expect(static_cast<bool>(stream), "failed to write test glTF");
}

void verifyImportedScene(const demo::GltfModel& model)
{
  expect(model.cameras.size() == 1, "expected one imported camera");
  expect(model.lights.size() == 2, "expected two imported lights");

  const demo::SceneCamera& camera = model.cameras.front();
  expect(camera.name == "MainCamera", "camera name was not preserved");
  expect(camera.nodeIndex == 0, "camera node binding was not preserved");
  expect(camera.type == demo::SceneCameraType::perspective, "camera type mismatch");
  expect(nearlyEqual(camera.aspectRatio, 1.5f), "camera aspect ratio mismatch");
  expect(nearlyEqual(camera.verticalFieldOfViewRadians, 0.7f), "camera yfov mismatch");
  expect(nearlyEqual(camera.nearPlane, 0.2f), "camera near plane mismatch");
  expect(nearlyEqual(camera.farPlane, 500.0f), "camera far plane mismatch");

  shaderio::CameraUniforms uniforms{};
  const demo::clipspace::ProjectionConvention convention =
      demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
  expect(demo::populatePrimarySceneCameraUniforms(
             model.cameras,
             std::span<const demo::SceneNode>{},
             model.nodes,
             demo::rhi::Extent2D{1920, 1080},
             convention,
             uniforms),
         "failed to resolve imported camera");
  expect(nearlyEqual(uniforms.cameraPosition.x, 1.0f)
      && nearlyEqual(uniforms.cameraPosition.y, 2.0f)
      && nearlyEqual(uniforms.cameraPosition.z, 3.0f),
      "camera node transform was not applied");
  expect(nearlyEqual(demo::clipspace::extractNearPlane(uniforms.projection, convention), 0.2f),
         "resolved perspective near plane mismatch");
  const float resolvedFarPlane = demo::clipspace::extractFarPlane(uniforms.projection, convention);
  expect(nearlyEqual(resolvedFarPlane, 500.0f, 0.05f),
         "resolved perspective far plane mismatch");

  demo::DirectionalLightSettings lightSettings{};
  expect(demo::applyPrimarySceneDirectionalLight(
             model.lights,
             std::span<const demo::SceneNode>{},
             model.nodes,
             lightSettings),
         "scene lights should be authoritative");
  expect(nearlyEqual(lightSettings.direction.x, 0.0f)
      && nearlyEqual(lightSettings.direction.y, 0.0f)
      && nearlyEqual(lightSettings.direction.z, -1.0f),
      "directional light node transform mismatch");
  expect(nearlyEqual(lightSettings.color.x, 1.0f)
      && nearlyEqual(lightSettings.color.y, 1.5f)
      && nearlyEqual(lightSettings.color.z, 2.0f),
      "directional light color/intensity mismatch");
  expect(glm::length(lightSettings.ambient) == 0.0f, "glTF lights must suppress fallback ambient light");
}

void testOrthographicCamera()
{
  demo::SceneCamera camera{};
  camera.nodeIndex = 0;
  camera.type = demo::SceneCameraType::orthographic;
  camera.horizontalMagnification = 3.0f;
  camera.verticalMagnification = 2.0f;
  camera.nearPlane = 0.5f;
  camera.farPlane = 50.0f;

  demo::SceneNode node{};
  node.worldTransform = glm::translate(glm::mat4(1.0f), glm::vec3(7.0f, 8.0f, 9.0f));

  shaderio::CameraUniforms uniforms{};
  const demo::clipspace::ProjectionConvention convention =
      demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
  expect(demo::populatePrimarySceneCameraUniforms(
             std::span<const demo::SceneCamera>(&camera, 1),
             std::span<const demo::SceneNode>(&node, 1),
             std::span<const demo::GltfNodeData>{},
             demo::rhi::Extent2D{800, 600},
             convention,
             uniforms),
         "failed to resolve orthographic camera");
  expect(demo::clipspace::isOrthographicProjection(uniforms.projection),
         "orthographic projection was not preserved");
  expect(nearlyEqual(demo::clipspace::extractNearPlane(uniforms.projection, convention), 0.5f),
         "orthographic near plane mismatch");
  expect(nearlyEqual(demo::clipspace::extractFarPlane(uniforms.projection, convention), 50.0f),
         "orthographic far plane mismatch");
}

void testSceneCameraNavigation()
{
  demo::SceneCamera camera{};
  camera.nodeIndex = 0;
  camera.type = demo::SceneCameraType::orthographic;
  camera.horizontalMagnification = 3.0f;
  camera.verticalMagnification = 2.0f;
  camera.nearPlane = 0.5f;
  camera.farPlane = 50.0f;

  demo::SceneNode node{};
  node.worldTransform = glm::translate(glm::mat4(1.0f), glm::vec3(7.0f, 8.0f, 9.0f))
                      * glm::rotate(glm::mat4(1.0f), 0.35f, glm::vec3(0.0f, 0.0f, 1.0f));

  shaderio::CameraUniforms baseUniforms{};
  const demo::clipspace::ProjectionConvention convention =
      demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
  expect(demo::populatePrimarySceneCameraUniforms(
             std::span<const demo::SceneCamera>(&camera, 1),
             std::span<const demo::SceneNode>(&node, 1),
             std::span<const demo::GltfNodeData>{},
             demo::rhi::Extent2D{800, 600},
             convention,
             baseUniforms),
         "failed to resolve scene camera for navigation");

  const glm::mat4 localNavigationWorld =
      glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, -3.0f))
      * glm::rotate(glm::mat4(1.0f), -0.25f, glm::vec3(0.0f, 1.0f, 0.0f));
  shaderio::CameraUniforms navigatedUniforms{};
  demo::populateNavigatedSceneCameraUniforms(
      baseUniforms, glm::inverse(localNavigationWorld), navigatedUniforms);

  const glm::mat4 expectedWorld = node.worldTransform * localNavigationWorld;
  expect(matricesNearlyEqual(navigatedUniforms.view, glm::inverse(expectedWorld)),
         "scene camera navigation did not preserve the base transform");
  expect(matricesNearlyEqual(navigatedUniforms.projection, baseUniforms.projection),
         "scene camera navigation changed the glTF projection");
  expect(nearlyEqual(navigatedUniforms.cameraPosition.x, expectedWorld[3].x)
      && nearlyEqual(navigatedUniforms.cameraPosition.y, expectedWorld[3].y)
      && nearlyEqual(navigatedUniforms.cameraPosition.z, expectedWorld[3].z),
      "scene camera navigation did not update the world position");
  expect(glm::length(navigatedUniforms.cameraPosition - baseUniforms.cameraPosition) > 0.1f,
         "scene camera navigation did not move the camera");
}

void testImportAndCacheRoundTrips()
{
  TemporaryDirectory temporaryDirectory;
  const std::filesystem::path sourcePath = temporaryDirectory.path / "scene.gltf";
  writeTestScene(sourcePath);

  demo::GltfLoader loader;
  demo::GltfModel model;
  expect(loader.load(sourcePath.string(), model), loader.getLastError().c_str());
  verifyImportedScene(model);

  const std::filesystem::path cachePath = temporaryDirectory.path / "scene.gltfcache";
  demo::SceneCacheSerializer cacheSerializer;
  expect(cacheSerializer.saveCache(cachePath, model, sourcePath), cacheSerializer.getLastError().c_str());
  demo::GltfModel cachedModel;
  expect(cacheSerializer.loadCache(cachePath, cachedModel), cacheSerializer.getLastError().c_str());
  verifyImportedScene(cachedModel);

  demo::SceneAsset asset = demo::SceneAssetBuilder::build(model);
  const demo::SceneAssetValidationResult validation = demo::validateSceneAssetView(demo::makeSceneAssetView(asset));
  expect(validation.valid, validation.error.c_str());

  const std::filesystem::path assetPath = temporaryDirectory.path / "scene.sceneasset";
  demo::SceneAssetSerializer assetSerializer;
  expect(assetSerializer.save(assetPath, asset, sourcePath), assetSerializer.getLastError().c_str());
  demo::SceneAsset cachedAsset;
  expect(assetSerializer.load(assetPath, cachedAsset), assetSerializer.getLastError().c_str());
  expect(cachedAsset.cameras.size() == 1, "scene asset cache lost the camera");
  expect(cachedAsset.lights.size() == 2, "scene asset cache lost the lights");

  shaderio::CameraUniforms uniforms{};
  const demo::clipspace::ProjectionConvention convention =
      demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
  expect(demo::populatePrimarySceneCameraUniforms(
             cachedAsset.cameras,
             cachedAsset.nodes,
             std::span<const demo::GltfNodeData>{},
             demo::rhi::Extent2D{1920, 1080},
             convention,
             uniforms),
         "scene asset cache camera could not be resolved");
  expect(nearlyEqual(uniforms.cameraPosition.x, 1.0f), "scene asset cache camera transform mismatch");
}

}  // namespace

int main()
{
  try {
    testImportAndCacheRoundTrips();
    testOrthographicCamera();
    testSceneCameraNavigation();
    std::cout << "scene import defaults tests passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "scene import defaults tests failed: " << error.what() << '\n';
    return 1;
  }
}
