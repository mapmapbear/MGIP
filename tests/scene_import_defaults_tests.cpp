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
  "materials": [{
    "name": "StrongEmitter",
    "emissiveFactor": [0.25, 0.5, 1.0],
    "extensions": {
      "KHR_materials_emissive_strength": {"emissiveStrength": 4.0}
    }
  }],
  "extensionsUsed": ["KHR_lights_punctual", "KHR_materials_emissive_strength"],
  "extensions": {
    "KHR_lights_punctual": {
      "lights": [
        {"name": "Sun", "type": "directional", "color": [0.5, 0.75, 1.0], "intensity": 34150.0},
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
  expect(model.materials.size() == 1, "expected one imported material");

  const glm::vec3 emissiveFactor = model.materials.front().emissiveFactor;
  expect(nearlyEqual(emissiveFactor.x, 1.0f)
      && nearlyEqual(emissiveFactor.y, 2.0f)
      && nearlyEqual(emissiveFactor.z, 4.0f),
      "KHR_materials_emissive_strength was not applied to the emissive factor");

  const demo::SceneCamera& camera = model.cameras.front();
  expect(camera.name == "MainCamera", "camera name was not preserved");
  expect(camera.nodeIndex == 0, "camera node binding was not preserved");
  expect(camera.type == demo::SceneCameraType::perspective, "camera type mismatch");
  expect(nearlyEqual(camera.aspectRatio, 1.5f), "camera aspect ratio mismatch");
  expect(nearlyEqual(camera.verticalFieldOfViewRadians, 0.7f), "camera yfov mismatch");
  expect(nearlyEqual(camera.nearPlane, 0.2f), "camera near plane mismatch");
  expect(nearlyEqual(camera.farPlane, 500.0f), "camera far plane mismatch");

  const demo::SceneLight& directionalLight = model.lights[0];
  expect(directionalLight.type == demo::SceneLightType::directional,
         "expected the first light to be directional");
  expect(nearlyEqual(directionalLight.intensity, 50.0f),
         "directional light lux was not converted to Blender-compatible intensity");
  const demo::SceneLight& pointLight = model.lights[1];
  expect(pointLight.type == demo::SceneLightType::point,
         "expected the second light to be a point light");
  expect(nearlyEqual(pointLight.intensity, 4.0f),
         "point light intensity must not use the directional-light conversion");

  shaderio::CameraUniforms uniforms{};
  const demo::clipspace::ProjectionConvention convention =
      demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
  const glm::mat4 originalProjection = demo::clipspace::makePerspectiveProjection(
      glm::radians(55.0f), 16.0f / 9.0f, 0.35f, 250.0f, convention);
  expect(demo::populatePrimarySceneCameraUniforms(
             model.cameras,
             std::span<const demo::SceneNode>{},
             model.nodes,
             originalProjection,
             uniforms),
         "failed to resolve imported camera");
  expect(nearlyEqual(uniforms.cameraPosition.x, 1.0f)
      && nearlyEqual(uniforms.cameraPosition.y, 2.0f)
      && nearlyEqual(uniforms.cameraPosition.z, 3.0f),
      "camera node transform was not applied");
  const glm::mat4 expectedProjection = demo::clipspace::makePerspectiveProjection(
      camera.verticalFieldOfViewRadians,
      camera.aspectRatio,
      camera.nearPlane,
      camera.farPlane,
      convention);
  expect(matricesNearlyEqual(uniforms.projection, expectedProjection),
         "glTF camera projection parameters were not applied");

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
  expect(nearlyEqual(lightSettings.color.x, 25.0f)
      && nearlyEqual(lightSettings.color.y, 37.5f)
      && nearlyEqual(lightSettings.color.z, 50.0f),
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
  const glm::mat4 originalProjection = demo::clipspace::makePerspectiveProjection(
      glm::radians(45.0f), 4.0f / 3.0f, 0.1f, 100.0f, convention);
  expect(demo::populatePrimarySceneCameraUniforms(
             std::span<const demo::SceneCamera>(&camera, 1),
             std::span<const demo::SceneNode>(&node, 1),
             std::span<const demo::GltfNodeData>{},
             originalProjection,
             uniforms),
         "failed to resolve orthographic camera");
  const glm::mat4 expectedProjection = demo::clipspace::makeOrthographicProjection(
      -camera.horizontalMagnification,
      camera.horizontalMagnification,
      -camera.verticalMagnification,
      camera.verticalMagnification,
      camera.nearPlane,
      camera.farPlane,
      convention);
  expect(matricesNearlyEqual(uniforms.projection, expectedProjection),
         "orthographic glTF camera projection parameters were not applied");
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
  const glm::mat4 sceneCameraPose =
      glm::translate(glm::mat4(1.0f), glm::vec3(7.0f, 8.0f, 9.0f))
      * glm::rotate(glm::mat4(1.0f), 0.35f, glm::vec3(0.0f, 0.0f, 1.0f));
  node.worldTransform = sceneCameraPose
                      * glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, 4.0f));

  shaderio::CameraUniforms baseUniforms{};
  const demo::clipspace::ProjectionConvention convention =
      demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
  const glm::mat4 originalProjection = demo::clipspace::makePerspectiveProjection(
      glm::radians(50.0f), 4.0f / 3.0f, 0.2f, 300.0f, convention);
  expect(demo::populatePrimarySceneCameraUniforms(
             std::span<const demo::SceneCamera>(&camera, 1),
             std::span<const demo::SceneNode>(&node, 1),
             std::span<const demo::GltfNodeData>{},
             originalProjection,
             baseUniforms),
         "failed to resolve scene camera for navigation");
  expect(matricesNearlyEqual(baseUniforms.view, glm::inverse(sceneCameraPose)),
         "scene camera node scale leaked into the flight camera pose");

  const glm::mat4 localNavigationWorld =
      glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, -3.0f))
      * glm::rotate(glm::mat4(1.0f), -0.25f, glm::vec3(0.0f, 1.0f, 0.0f));
  shaderio::CameraUniforms navigatedUniforms{};
  demo::populateNavigatedSceneCameraUniforms(
      baseUniforms, glm::inverse(localNavigationWorld), navigatedUniforms);

  const glm::mat4 expectedWorld = sceneCameraPose * localNavigationWorld;
  expect(matricesNearlyEqual(navigatedUniforms.view, glm::inverse(expectedWorld)),
         "scene camera navigation did not preserve the base transform");
  expect(matricesNearlyEqual(navigatedUniforms.projection, baseUniforms.projection),
         "scene camera navigation changed the flight camera projection");
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
  expect(cachedAsset.materials.size() == 1, "scene asset cache lost the material");
  expect(nearlyEqual(cachedAsset.materials.front().emissiveFactor.x, 1.0f)
      && nearlyEqual(cachedAsset.materials.front().emissiveFactor.y, 2.0f)
      && nearlyEqual(cachedAsset.materials.front().emissiveFactor.z, 4.0f),
      "scene asset cache lost the emissive strength");

  shaderio::CameraUniforms uniforms{};
  const demo::clipspace::ProjectionConvention convention =
      demo::clipspace::getProjectionConvention(demo::clipspace::BackendConvention::vulkan);
  const glm::mat4 originalProjection = demo::clipspace::makePerspectiveProjection(
      glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f, convention);
  expect(demo::populatePrimarySceneCameraUniforms(
             cachedAsset.cameras,
             cachedAsset.nodes,
             std::span<const demo::GltfNodeData>{},
             originalProjection,
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
