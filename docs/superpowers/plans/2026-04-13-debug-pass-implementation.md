# DebugPass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace ShadowDebugPass with unified DebugPass for comprehensive debug visualization (bounds, lights, frustum, normals, stats)

**Architecture:** Primitive Registry pattern - DebugPass orchestrates, each primitive is an independent class implementing IDebugPrimitive interface. Geometry collected into transient buffer and drawn as line overlay on lit scene. Stats drawn via ImGui overlay.

**Tech Stack:** C++17, GLM, Vulkan RHI, ImGui, Slang shaders

---

## File Structure

```
New files:
- render/DebugRegistry.h          (registry + primitive interface + settings)
- render/passes/DebugPrimitives.h (6 primitive implementations)

Modified files:
- render/Renderer.h               (add DebugRegistry, getDebugRegistry())
- render/Renderer.cpp             (register primitives in init)
- render/passes/ShadowDebugPass.h → rename to DebugPass.h
- render/passes/ShadowDebugPass.cpp → rename to DebugPass.cpp, rewrite execute
- app/MinimalLatestApp.h          (add DebugSettings member, drawDebugPanel)
- shaders/shader_io.h             (add DebugSettings to RenderParams)
```

---

### Task 1: Create DebugRegistry Header

**Files:**
- Create: `render/DebugRegistry.h`

- [ ] **Step 1: Write DebugRegistry header with primitive interface and settings**

```cpp
#pragma once

#include "../common/Common.h"
#include "Pass.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace demo {

// Forward declarations
class Renderer;
struct GltfUploadResult;

// Debug vertex format: position + color (28 bytes)
struct DebugVertex {
    float position[3];   // World space position
    float color[4];      // RGBA color
};

// Debug settings controlled via ImGui
struct DebugSettings {
    // Geometry visualization
    bool showBounds{false};
    bool showBoundingSpheres{false};
    bool showLights{false};
    bool showFrustum{false};
    bool showNormals{false};
    bool showStats{true};  // Default on
    
    // Light visualization options
    int selectedLightIndex{-1};          // -1 = all lights
    float lightVisualizationScale{1.0f};
    
    // Bounds visualization options
    float boundsScale{1.0f};
    bool boundsForAllMeshes{true};
    
    // Normal visualization options
    float normalVisualizationLength{0.1f};
    int normalVisualizationDensity{4};  // Pixels per normal
    
    // Frustum options
    bool showCameraFrustum{true};
    bool showShadowCascades{true};
    int cascadeIndex{-1};                // -1 = all cascades
};

// Base interface for debug primitives
class IDebugPrimitive {
public:
    virtual ~IDebugPrimitive() = default;
    
    // Check if primitive is enabled based on settings
    virtual bool isEnabled(const DebugSettings& settings) const = 0;
    
    // Collect geometry data into buffer, return vertex count
    virtual uint32_t collectData(
        const PassContext& context,
        DebugVertex* vertexPtr
    ) const = 0;
    
    // Get primitive topology (all use lines except stats)
    virtual rhi::PrimitiveTopology getTopology() const {
        return rhi::PrimitiveTopology::lineList;
    }
    
    // Name for registry key and ImGui labels
    virtual const char* getName() const = 0;
    
    // Stats primitive special: draws ImGui directly (no geometry)
    virtual bool hasImGuiOverlay() const { return false; }
    virtual void drawImGui(const PassContext& context) const {}
};

// Registry managing primitive factories
class DebugRegistry {
public:
    using PrimitiveFactory = std::function<std::unique_ptr<IDebugPrimitive>()>;
    
    void registerPrimitive(const std::string& name, PrimitiveFactory factory) {
        m_factories[name] = factory;
    }
    
    // Create all primitives (caller filters by enabled)
    const std::map<std::string, PrimitiveFactory>& getAllFactories() const {
        return m_factories;
    }
    
    // Utility: check if any geometry primitive is enabled
    static bool hasGeometryPrimitives(const DebugSettings& settings) {
        return settings.showBounds || settings.showBoundingSpheres ||
               settings.showLights || settings.showFrustum || settings.showNormals;
    }
    
private:
    std::map<std::string, PrimitiveFactory> m_factories;
};

}  // namespace demo
```

- [ ] **Step 2: Verify header compiles**

Run: `cmake --build build --config Release 2>&1 | grep -E "(error|DebugRegistry)" || echo "Header added successfully"`
Expected: No errors (header not yet included)

- [ ] **Step 3: Commit**

```bash
git add render/DebugRegistry.h
git commit -m "feat(debug): add DebugRegistry header with primitive interface

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 2: Create DebugPrimitives Header

**Files:**
- Create: `render/passes/DebugPrimitives.h`

- [ ] **Step 1: Write all 6 primitive implementations**

```cpp
#pragma once

#include "../DebugRegistry.h"
#include "../Renderer.h"
#include "../ShadowResources.h"
#include "../MeshPool.h"
#include "../../shaders/shader_io.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstring>

namespace demo {

//==============================================================================
// BoundingBoxPrimitive
//==============================================================================

class BoundingBoxPrimitive : public IDebugPrimitive {
public:
    bool isEnabled(const DebugSettings& s) const override { return s.showBounds; }
    const char* getName() const override { return "Bounding Boxes"; }
    
    uint32_t collectData(const PassContext& ctx, DebugVertex* vtx) const override {
        if(ctx.gltfModel == nullptr) return 0;
        
        MeshPool& meshPool = ctx.params->renderer->getMeshPool();
        uint32_t count = 0;
        
        for(size_t i = 0; i < ctx.gltfModel->meshes.size(); ++i) {
            const MeshRecord* mesh = meshPool.tryGet(ctx.gltfModel->meshes[i]);
            if(mesh == nullptr) continue;
            
            // Get mesh AABB from vertex bounds (stored or computed)
            // For now: use transform translation as center, unit box
            const glm::vec3 center = glm::vec3(mesh->transform[3]);
            const float scale = 1.0f;  // TODO: compute from actual vertex bounds
            
            // Box corners
            const glm::vec3 corners[8] = {
                center + glm::vec3(-scale, -scale, -scale),
                center + glm::vec3( scale, -scale, -scale),
                center + glm::vec3(-scale,  scale, -scale),
                center + glm::vec3( scale,  scale, -scale),
                center + glm::vec3(-scale, -scale,  scale),
                center + glm::vec3( scale, -scale,  scale),
                center + glm::vec3(-scale,  scale,  scale),
                center + glm::vec3( scale,  scale,  scale),
            };
            
            // Line indices: 12 edges = 24 vertices
            constexpr int edges[24] = {
                0,1, 1,3, 3,2, 2,0,  // bottom
                4,5, 5,7, 7,6, 6,4,  // top
                0,4, 1,5, 2,6, 3,7,  // verticals
            };
            
            const float color[4] = {1.0f, 1.0f, 0.0f, 1.0f};  // Yellow
            
            for(int j = 0; j < 24; ++j) {
                const glm::vec3& p = corners[edges[j]];
                vtx[count].position[0] = p.x;
                vtx[count].position[1] = p.y;
                vtx[count].position[2] = p.z;
                std::memcpy(vtx[count].color, color, sizeof(color));
                ++count;
            }
        }
        return count;
    }
};

//==============================================================================
// BoundingSpherePrimitive
//==============================================================================

class BoundingSpherePrimitive : public IDebugPrimitive {
public:
    bool isEnabled(const DebugSettings& s) const override { return s.showBoundingSpheres; }
    const char* getName() const override { return "Bounding Spheres"; }
    
    uint32_t collectData(const PassContext& ctx, DebugVertex* vtx) const override {
        if(ctx.gltfModel == nullptr) return 0;
        
        MeshPool& meshPool = ctx.params->renderer->getMeshPool();
        uint32_t count = 0;
        
        constexpr int kSegments = 32;
        const float color[4] = {0.0f, 1.0f, 1.0f, 1.0f};  // Cyan
        
        for(size_t i = 0; i < ctx.gltfModel->meshes.size(); ++i) {
            const MeshRecord* mesh = meshPool.tryGet(ctx.gltfModel->meshes[i]);
            if(mesh == nullptr) continue;
            
            const glm::vec3 center = glm::vec3(mesh->transform[3]);
            const float radius = 1.0f;  // TODO: compute from actual bounds
            
            // Draw 3 circles: XZ, XY, YZ planes
            for(int plane = 0; plane < 3; ++plane) {
                for(int j = 0; j < kSegments; ++j) {
                    const float angle0 = 2.0f * 3.14159f * j / kSegments;
                    const float angle1 = 2.0f * 3.14159f * (j + 1) / kSegments;
                    
                    glm::vec3 p0, p1;
                    if(plane == 0) {  // XZ plane (Y up)
                        p0 = center + glm::vec3(radius * std::cos(angle0), 0.0f, radius * std::sin(angle0));
                        p1 = center + glm::vec3(radius * std::cos(angle1), 0.0f, radius * std::sin(angle1));
                    } else if(plane == 1) {  // XY plane (Z forward)
                        p0 = center + glm::vec3(radius * std::cos(angle0), radius * std::sin(angle0), 0.0f);
                        p1 = center + glm::vec3(radius * std::cos(angle1), radius * std::sin(angle1), 0.0f);
                    } else {  // YZ plane (X right)
                        p0 = center + glm::vec3(0.0f, radius * std::cos(angle0), radius * std::sin(angle0));
                        p1 = center + glm::vec3(0.0f, radius * std::cos(angle1), radius * std::sin(angle1));
                    }
                    
                    vtx[count].position[0] = p0.x;
                    vtx[count].position[1] = p0.y;
                    vtx[count].position[2] = p0.z;
                    std::memcpy(vtx[count].color, color, sizeof(color));
                    ++count;
                    
                    vtx[count].position[0] = p1.x;
                    vtx[count].position[1] = p1.y;
                    vtx[count].position[2] = p1.z;
                    std::memcpy(vtx[count].color, color, sizeof(color));
                    ++count;
                }
            }
        }
        return count;
    }
};

//==============================================================================
// LightVisualizerPrimitive
//==============================================================================

class LightVisualizerPrimitive : public IDebugPrimitive {
public:
    bool isEnabled(const DebugSettings& s) const override { return s.showLights; }
    const char* getName() const override { return "Light Visualization"; }
    
    uint32_t collectData(const PassContext& ctx, DebugVertex* vtx) const override {
        // Draw light direction arrow from shadow uniforms
        ShadowResources& shadow = ctx.params->renderer->getShadowResources();
        const shaderio::ShadowUniforms* shadowData = shadow.getShadowUniformsData();
        if(shadowData == nullptr) return 0;
        
        uint32_t count = 0;
        
        // Light position (computed from scene center + direction)
        const glm::vec3 sceneCenter(0.0f, 5.0f, 0.0f);
        const glm::vec3 lightDir = shadowData->lightDirection;  // Direction TO scene
        const float lightDist = 50.0f;
        const glm::vec3 lightPos = sceneCenter - lightDir * lightDist;
        
        // Arrow: line from lightPos toward sceneCenter
        const float arrowLen = 10.0f;
        const glm::vec3 arrowEnd = lightPos + lightDir * arrowLen;
        
        const float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // White
        
        // Main direction line
        vtx[count].position[0] = lightPos.x;
        vtx[count].position[1] = lightPos.y;
        vtx[count].position[2] = lightPos.z;
        std::memcpy(vtx[count].color, color, sizeof(color));
        ++count;
        
        vtx[count].position[0] = arrowEnd.x;
        vtx[count].position[1] = arrowEnd.y;
        vtx[count].position[2] = arrowEnd.z;
        std::memcpy(vtx[count].color, color, sizeof(color));
        ++count;
        
        // Cross at light position (3 axes)
        const float crossSize = 2.0f;
        const float crossColors[3][4] = {
            {1.0f, 0.0f, 0.0f, 1.0f},  // X: red
            {0.0f, 1.0f, 0.0f, 1.0f},  // Y: green
            {0.0f, 0.0f, 1.0f, 1.0f},  // Z: blue
        };
        
        for(int axis = 0; axis < 3; ++axis) {
            glm::vec3 axisDir(0.0f);
            axisDir[axis] = crossSize;
            
            vtx[count].position[0] = lightPos.x - axisDir.x;
            vtx[count].position[1] = lightPos.y - axisDir.y;
            vtx[count].position[2] = lightPos.z - axisDir.z;
            std::memcpy(vtx[count].color, crossColors[axis], sizeof(crossColors[axis]));
            ++count;
            
            vtx[count].position[0] = lightPos.x + axisDir.x;
            vtx[count].position[1] = lightPos.y + axisDir.y;
            vtx[count].position[2] = lightPos.z + axisDir.z;
            std::memcpy(vtx[count].color, crossColors[axis], sizeof(crossColors[axis]));
            ++count;
        }
        
        return count;
    }
};

//==============================================================================
// FrustumWireframePrimitive
//==============================================================================

class FrustumWireframePrimitive : public IDebugPrimitive {
public:
    bool isEnabled(const DebugSettings& s) const override { return s.showFrustum; }
    const char* getName() const override { return "Frustum Wireframe"; }
    
    uint32_t collectData(const PassContext& ctx, DebugVertex* vtx) const override {
        if(ctx.params == nullptr || ctx.params->cameraUniforms == nullptr) return 0;
        
        uint32_t count = 0;
        
        // Camera frustum
        if(ctx.params->debugSettings.showCameraFrustum) {
            const glm::mat4 invViewProj = glm::inverse(ctx.params->cameraUniforms->viewProjection);
            
            // NDC corners (Vulkan Z in [0,1])
            constexpr glm::vec4 ndc[8] = {
                {-1,-1, 0,1}, { 1,-1, 0,1}, {-1, 1, 0,1}, { 1, 1, 0,1},  // near
                {-1,-1, 1,1}, { 1,-1, 1,1}, {-1, 1, 1,1}, { 1, 1, 1,1},  // far
            };
            
            glm::vec3 world[8];
            for(int i = 0; i < 8; ++i) {
                glm::vec4 w = invViewProj * ndc[i];
                world[i] = glm::vec3(w / w.w);
            }
            
            const float color[4] = {1.0f, 0.0f, 1.0f, 1.0f};  // Magenta
            count += writeFrustumLines(vtx + count, world, color);
        }
        
        // Shadow cascade frustums
        if(ctx.params->debugSettings.showShadowCascades) {
            ShadowResources& shadow = ctx.params->renderer->getShadowResources();
            const shaderio::ShadowUniforms* shadowData = shadow.getShadowUniformsData();
            if(shadowData) {
                const float cascadeColors[4][4] = {
                    {1.0f, 0.0f, 0.0f, 1.0f},  // 0: red
                    {0.0f, 1.0f, 0.0f, 1.0f},  // 1: green
                    {0.0f, 0.0f, 1.0f, 1.0f},  // 2: blue
                    {0.0f, 1.0f, 1.0f, 1.0f},  // 3: cyan
                };
                
                const int targetCascade = ctx.params->debugSettings.cascadeIndex;
                for(int c = 0; c < shaderio::LCascadeCount; ++c) {
                    if(targetCascade >= 0 && targetCascade != c) continue;
                    
                    const glm::mat4 invLightViewProj = glm::inverse(
                        shadowData->cascades[c].viewProjectionMatrix);
                    
                    glm::vec3 world[8];
                    for(int i = 0; i < 8; ++i) {
                        glm::vec4 w = invLightViewProj * ndc[i];
                        world[i] = glm::vec3(w / w.w);
                    }
                    
                    count += writeFrustumLines(vtx + count, world, cascadeColors[c]);
                }
            }
        }
        
        return count;
    }
    
private:
    // NDC corners for frustum (Vulkan Z [0,1])
    static constexpr glm::vec4 ndc[8] = {
        {-1,-1, 0,1}, { 1,-1, 0,1}, {-1, 1, 0,1}, { 1, 1, 0,1},
        {-1,-1, 1,1}, { 1,-1, 1,1}, {-1, 1, 1,1}, { 1, 1, 1,1},
    };
    
    static uint32_t writeFrustumLines(DebugVertex* vtx, const glm::vec3 corners[8], const float color[4]) {
        constexpr int edges[24] = {
            0,1, 1,3, 3,2, 2,0,  // near
            4,5, 5,7, 7,6, 6,4,  // far
            0,4, 1,5, 2,6, 3,7,  // connections
        };
        for(int i = 0; i < 24; ++i) {
            const glm::vec3& p = corners[edges[i]];
            vtx[i].position[0] = p.x;
            vtx[i].position[1] = p.y;
            vtx[i].position[2] = p.z;
            std::memcpy(vtx[i].color, color, sizeof(float[4]));
        }
        return 24;
    }
};

//==============================================================================
// NormalOverlayPrimitive (placeholder - needs GBuffer access)
//==============================================================================

class NormalOverlayPrimitive : public IDebugPrimitive {
public:
    bool isEnabled(const DebugSettings& s) const override { return s.showNormals; }
    const char* getName() const override { return "Normal Overlay"; }
    
    uint32_t collectData(const PassContext& ctx, DebugVertex* vtx) const override {
        // TODO: Read GBuffer normal texture and draw lines
        // This requires texture readback or compute pass
        // For now: return 0 (placeholder)
        return 0;
    }
};

//==============================================================================
// StatsOverlayPrimitive (ImGui only, no geometry)
//==============================================================================

class StatsOverlayPrimitive : public IDebugPrimitive {
public:
    bool isEnabled(const DebugSettings& s) const override { return s.showStats; }
    const char* getName() const override { return "Stats Overlay"; }
    
    uint32_t collectData(const PassContext& ctx, DebugVertex* vtx) const override {
        return 0;  // No geometry
    }
    
    bool hasImGuiOverlay() const override { return true; }
    
    void drawImGui(const PassContext& ctx) const override {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.35f);
        
        ImGui::Begin("##StatsOverlay", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav);
        
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::Text("Frame: %.2f ms", 1000.0f / ImGui::GetIO().Framerate);
        
        if(ctx.gltfModel) {
            ImGui::Text("Meshes: %zu", ctx.gltfModel->meshes.size());
            ImGui::Text("Materials: %zu", ctx.gltfModel->materials.size());
            ImGui::Text("Textures: %zu", ctx.gltfModel->textures.size());
        }
        
        if(ctx.params && ctx.params->cameraUniforms) {
            const glm::vec3& camPos = ctx.params->cameraUniforms->cameraPosition;
            ImGui::Text("Camera: (%.1f, %.1f, %.1f)", camPos.x, camPos.y, camPos.z);
        }
        
        ImGui::End();
    }
};

}  // namespace demo
```

- [ ] **Step 2: Verify header compiles**

Run: `cmake --build build --config Release 2>&1 | grep -E "(error|DebugPrimitives)" || echo "Header added successfully"`
Expected: May have missing includes - fix inline

- [ ] **Step 3: Commit**

```bash
git add render/passes/DebugPrimitives.h
git commit -m "feat(debug): add 6 debug primitive implementations

BoundingBox, BoundingSphere, Light, Frustum, Normal (placeholder), Stats

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 3: Add DebugSettings to RenderParams

**Files:**
- Modify: `shaders/shader_io.h`

- [ ] **Step 1: Add DebugSettings struct to shader_io.h**

Find the `RenderParams` struct in shader_io.h (around line 53-69). Add before it:

```cpp
// DebugSettings struct (from DebugRegistry.h, duplicated here for shader access)
struct DebugSettings {
  // Geometry visualization
  bool showBounds{false};
  bool showBoundingSpheres{false};
  bool showLights{false};
  bool showFrustum{false};
  bool showNormals{false};
  bool showStats{true};
  
  // Visualization options
  int selectedLightIndex{-1};
  float lightVisualizationScale{1.0f};
  float boundsScale{1.0f};
  bool boundsForAllMeshes{true};
  float normalVisualizationLength{0.1f};
  int normalVisualizationDensity{4};
  bool showCameraFrustum{true};
  bool showShadowCascades{true};
  int cascadeIndex{-1};
};
```

- [ ] **Step 2: Add debugSettings field to RenderParams**

In RenderParams struct, add after `shadowDebugMode`:

```cpp
  // Debug visualization settings
  DebugSettings debugSettings{};
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --config Release 2>&1 | tail -20`
Expected: Build succeeds, shader_io.h changes propagated

- [ ] **Step 4: Commit**

```bash
git add shaders/shader_io.h
git commit -m "feat(debug): add DebugSettings to shader_io.h and RenderParams

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 4: Update Renderer to Initialize DebugRegistry

**Files:**
- Modify: `render/Renderer.h`
- Modify: `render/Renderer.cpp`

- [ ] **Step 1: Add DebugRegistry to Renderer.h**

Add include near top (after ShadowResources.h include):

```cpp
#include "DebugRegistry.h"
```

Add member variable in private section (after `m_shadowResources`):

```cpp
  DebugRegistry m_debugRegistry;
```

Add public accessor (near `getShadowResources()`):

```cpp
  DebugRegistry& getDebugRegistry() { return m_debugRegistry; }
```

- [ ] **Step 2: Add primitive registration in Renderer.cpp init()**

In `Renderer::init()`, after shadow resources init, add:

```cpp
  // Initialize debug primitives
  m_debugRegistry.registerPrimitive("bounds", []() {
      return std::make_unique<BoundingBoxPrimitive>();
  });
  m_debugRegistry.registerPrimitive("spheres", []() {
      return std::make_unique<BoundingSpherePrimitive>();
  });
  m_debugRegistry.registerPrimitive("lights", []() {
      return std::make_unique<LightVisualizerPrimitive>();
  });
  m_debugRegistry.registerPrimitive("frustum", []() {
      return std::make_unique<FrustumWireframePrimitive>();
  });
  m_debugRegistry.registerPrimitive("normals", []() {
      return std::make_unique<NormalOverlayPrimitive>();
  });
  m_debugRegistry.registerPrimitive("stats", []() {
      return std::make_unique<StatsOverlayPrimitive>();
  });
```

Add include at top of Renderer.cpp:

```cpp
#include "passes/DebugPrimitives.h"
```

- [ ] **Step 3: Build and verify**

Run: `cmake --build build --config Release 2>&1 | tail -20`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
git add render/Renderer.h render/Renderer.cpp
git commit -m "feat(debug): register debug primitives in Renderer

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 5: Rename ShadowDebugPass to DebugPass

**Files:**
- Rename: `render/passes/ShadowDebugPass.h` → `render/passes/DebugPass.h`
- Rename: `render/passes/ShadowDebugPass.cpp` → `render/passes/DebugPass.cpp`
- Modify: `render/Renderer.h` (update include and member)

- [ ] **Step 1: Rename files**

```bash
cd H:/GitHub/VKDemo
git mv render/passes/ShadowDebugPass.h render/passes/DebugPass.h
git mv render/passes/ShadowDebugPass.cpp render/passes/DebugPass.cpp
```

- [ ] **Step 2: Update class name in DebugPass.h**

Change:
```cpp
class ShadowDebugPass : public RenderPassNode {
```
To:
```cpp
class DebugPass : public RenderPassNode {
```

Change:
```cpp
    explicit ShadowDebugPass(Renderer* renderer);
```
To:
```cpp
    explicit DebugPass(Renderer* renderer);
```

Change:
```cpp
    [[nodiscard]] const char* getName() const override { return "ShadowDebugPass"; }
```
To:
```cpp
    [[nodiscard]] const char* getName() const override { return "DebugPass"; }
```

- [ ] **Step 3: Update Renderer.h include**

Change:
```cpp
#include "passes/ShadowDebugPass.h"
```
To:
```cpp
#include "passes/DebugPass.h"
```

Change member:
```cpp
  std::unique_ptr<ShadowDebugPass>     m_shadowDebugPass;
```
To:
```cpp
  std::unique_ptr<DebugPass>           m_debugPass;
```

- [ ] **Step 4: Build and verify compilation**

Run: `cmake --build build --config Release 2>&1 | tail -20`
Expected: Build succeeds with renamed files

- [ ] **Step 5: Commit**

```bash
git add render/passes/DebugPass.h render/passes/DebugPass.cpp render/Renderer.h
git commit -m "refactor(debug): rename ShadowDebugPass to DebugPass

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 6: Rewrite DebugPass execute()

**Files:**
- Modify: `render/passes/DebugPass.cpp`
- Modify: `render/passes/DebugPass.h`

- [ ] **Step 1: Update DebugPass.h to include DebugRegistry**

Add at top:
```cpp
#include "../DebugRegistry.h"
```

- [ ] **Step 2: Rewrite DebugPass.cpp execute() to use primitive registry**

Replace entire execute() function body with:

```cpp
void DebugPass::execute(const PassContext& context) const {
    if(m_renderer == nullptr || context.params == nullptr)
        return;
    
    const DebugSettings& settings = context.params->debugSettings;
    
    // Early exit if nothing enabled
    if(!DebugRegistry::hasGeometryPrimitives(settings) && !settings.showStats)
        return;
    
    context.cmd->beginEvent("DebugPass");
    
    // === GEOMETRY PASS ===
    if(DebugRegistry::hasGeometryPrimitives(settings) && context.transientAllocator != nullptr) {
        constexpr uint32_t kMaxDebugVertices = 4096;
        constexpr uint32_t kDebugVertexSize = sizeof(DebugVertex);  // 28 bytes
        
        const TransientAllocator::Allocation vertexAlloc =
            context.transientAllocator->allocate(kMaxDebugVertices * kDebugVertexSize, 16);
        
        DebugVertex* vertexPtr = static_cast<DebugVertex*>(vertexAlloc.cpuPtr);
        uint32_t totalVertexCount = 0;
        
        const DebugRegistry& registry = m_renderer->getDebugRegistry();
        for(const auto& [name, factory] : registry.getAllFactories()) {
            auto primitive = factory();
            if(primitive && primitive->isEnabled(settings) && !primitive->hasImGuiOverlay()) {
                uint32_t count = primitive->collectData(context, vertexPtr + totalVertexCount);
                totalVertexCount += count;
                if(totalVertexCount >= kMaxDebugVertices) break;  // Buffer overflow guard
            }
        }
        
        if(totalVertexCount > 0) {
            context.transientAllocator->flushAllocation(vertexAlloc, totalVertexCount * kDebugVertexSize);
            
            const rhi::Extent2D extent = context.params->viewportSize;
            beginOverlayRenderPass(context, extent);
            bindDebugPipeline(context);
            bindCameraDescriptorSet(context);
            
            const uint64_t vbHandle = context.transientAllocator->getBufferOpaque();
            const uint64_t vbOffset = vertexAlloc.offset;
            context.cmd->bindVertexBuffers(0, &vbHandle, &vbOffset, 1);
            
            context.cmd->draw(totalVertexCount, 1, 0, 0);
            context.cmd->endRenderPass();
        }
    }
    
    // === IMGUI OVERLAY ===
    if(settings.showStats) {
        auto statsPrimitive = std::make_unique<StatsOverlayPrimitive>();
        if(statsPrimitive->isEnabled(settings)) {
            statsPrimitive->drawImGui(context);
        }
    }
    
    context.cmd->endEvent();
}
```

- [ ] **Step 3: Add helper methods to DebugPass.cpp**

Add after execute():

```cpp
void DebugPass::beginOverlayRenderPass(const PassContext& context, rhi::Extent2D extent) const {
    const rhi::RenderTargetDesc colorTarget{
        .texture = {},
        .view = rhi::TextureViewHandle::fromNative(m_renderer->getOutputTextureView()),
        .state = rhi::ResourceState::general,
        .loadOp = rhi::LoadOp::load,
        .storeOp = rhi::StoreOp::store,
    };
    
    const rhi::DepthTargetDesc depthTarget{
        .texture = {},
        .view = rhi::TextureViewHandle::fromNative(m_renderer->getDepthTextureView()),
        .state = rhi::ResourceState::General,
        .loadOp = rhi::LoadOp::load,
        .storeOp = rhi::StoreOp::store,
    };
    
    const rhi::RenderPassDesc passDesc{
        .renderArea = {{0, 0}, extent},
        .colorTargets = &colorTarget,
        .colorTargetCount = 1,
        .depthTarget = &depthTarget,
    };
    context.cmd->beginRenderPass(passDesc);
    
    const rhi::Viewport viewport{0.0f, 0.0f,
        static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    const rhi::Rect2D scissor{{0, 0}, extent};
    context.cmd->setViewport(viewport);
    context.cmd->setScissor(scissor);
}

void DebugPass::bindDebugPipeline(const PassContext& context) const {
    PipelineHandle debugPipeline = m_renderer->getDebugLinePipelineHandle();
    if(!debugPipeline.isNull()) {
        VkPipeline nativePipeline = reinterpret_cast<VkPipeline>(
            m_renderer->getPipelineOpaque(debugPipeline, static_cast<uint32_t>(VK_PIPELINE_BIND_POINT_GRAPHICS)));
        vkCmdBindPipeline(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                          VK_PIPELINE_BIND_POINT_GRAPHICS, nativePipeline);
    }
}

void DebugPass::bindCameraDescriptorSet(const PassContext& context) const {
    const BindGroupHandle cameraBindGroupHandle = m_renderer->getCameraBindGroup(context.frameIndex);
    if(!cameraBindGroupHandle.isNull()) {
        const VkPipelineLayout pipelineLayout = reinterpret_cast<VkPipelineLayout>(m_renderer->getGBufferPipelineLayout());
        uint64_t cameraSetOpaque = m_renderer->getBindGroupDescriptorSet(cameraBindGroupHandle, BindGroupSetSlot::shaderSpecific);
        VkDescriptorSet cameraDescriptorSet = reinterpret_cast<VkDescriptorSet>(cameraSetOpaque);
        const uint32_t cameraDynamicOffset = 0;  // Camera at start of transient buffer
        vkCmdBindDescriptorSets(rhi::vulkan::getNativeCommandBuffer(*context.cmd),
                                VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                shaderio::LSetScene, 1, &cameraDescriptorSet, 1, &cameraDynamicOffset);
    }
}
```

- [ ] **Step 4: Add helper method declarations to DebugPass.h**

Add in private section after `m_renderer`:

```cpp
    void beginOverlayRenderPass(const PassContext& context, rhi::Extent2D extent) const;
    void bindDebugPipeline(const PassContext& context) const;
    void bindCameraDescriptorSet(const PassContext& context) const;
```

- [ ] **Step 5: Build and verify**

Run: `cmake --build build --config Release 2>&1 | tail -30`
Expected: Build succeeds

- [ ] **Step 6: Commit**

```bash
git add render/passes/DebugPass.h render/passes/DebugPass.cpp
git commit -m "feat(debug): rewrite DebugPass execute with primitive registry

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 7: Add Debug Panel to MinimalLatestApp

**Files:**
- Modify: `app/MinimalLatestApp.h`

- [ ] **Step 1: Add DebugSettings member to MinimalLatestApp**

In private section, add after `m_shadowDebugMode`:

```cpp
  // Debug visualization settings
  shaderio::DebugSettings m_debugSettings;
```

- [ ] **Step 2: Add drawDebugPanel() function**

Add in private section after `drawModelLoaderUI()`:

```cpp
  void drawDebugPanel();
```

- [ ] **Step 3: Implement drawDebugPanel() function**

Add after `drawModelLoaderUI()` implementation:

```cpp
inline void MinimalLatestApp::drawDebugPanel() {
    if(ImGui::CollapsingHeader("Debug Visualization")) {
        ImGui::Indent();
        
        // Geometry section
        ImGui::Text("Geometry:");
        ImGui::Checkbox("Bounding Boxes", &m_debugSettings.showBounds);
        if(m_debugSettings.showBounds) {
            ImGui::SliderFloat("Bounds Scale", &m_debugSettings.boundsScale, 0.5f, 2.0f);
        }
        
        ImGui::Checkbox("Bounding Spheres", &m_debugSettings.showBoundingSpheres);
        
        // Light section
        ImGui::Separator();
        ImGui::Text("Lights:");
        ImGui::Checkbox("Show Lights", &m_debugSettings.showLights);
        if(m_debugSettings.showLights) {
            ImGui::SliderFloat("Light Scale", &m_debugSettings.lightVisualizationScale, 0.1f, 3.0f);
        }
        
        // Frustum section
        ImGui::Separator();
        ImGui::Text("Frustum:");
        ImGui::Checkbox("Show Frustum", &m_debugSettings.showFrustum);
        if(m_debugSettings.showFrustum) {
            ImGui::Checkbox("Camera Frustum", &m_debugSettings.showCameraFrustum);
            ImGui::Checkbox("Shadow Cascades", &m_debugSettings.showShadowCascades);
            static const char* cascadeNames[] = {"All", "Cascade 0", "Cascade 1", "Cascade 2", "Cascade 3"};
            ImGui::Combo("Cascade", &m_debugSettings.cascadeIndex, cascadeNames, 5);
        }
        
        // Stats section
        ImGui::Separator();
        ImGui::Checkbox("Stats Overlay", &m_debugSettings.showStats);
        
        ImGui::Unindent();
    }
}
```

- [ ] **Step 4: Pass debugSettings to RenderParams**

In `run()` loop, add to `frameParams`:

```cpp
      frameParams.debugSettings = m_debugSettings;
```

- [ ] **Step 5: Call drawDebugPanel() in Settings window**

In Settings ImGui::Begin block, after shadow debug mode combo, add:

```cpp
        drawDebugPanel();
```

- [ ] **Step 6: Build and verify**

Run: `cmake --build build --config Release 2>&1 | tail -20`
Expected: Build succeeds

- [ ] **Step 7: Commit**

```bash
git add app/MinimalLatestApp.h
git commit -m "feat(debug): add debug visualization panel to MinimalLatestApp

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

### Task 8: Test and Verify Debug Visualization

**Files:**
- Test: Run application, verify each primitive

- [ ] **Step 1: Build application**

Run: `cmake --build build --config Release`
Expected: Build succeeds

- [ ] **Step 2: Run application and test stats overlay**

Run: `./build/Release/Demo.exe`
Check: Stats overlay appears at top-left showing FPS, frame time, mesh counts

- [ ] **Step 3: Test bounding box visualization**

Enable "Bounding Boxes" checkbox in Settings → Debug Visualization
Check: Yellow wireframe boxes appear around each mesh

- [ ] **Step 4: Test bounding sphere visualization**

Enable "Bounding Spheres" checkbox
Check: Cyan wireframe spheres (3 circles per mesh) appear

- [ ] **Step 5: Test light visualization**

Enable "Show Lights" checkbox
Check: White arrow and RGB axis cross appear at light position

- [ ] **Step 6: Test frustum visualization**

Enable "Show Frustum" checkbox
Check: Magenta camera frustum and colored cascade frustums appear

- [ ] **Step 7: Final commit**

```bash
git add -A
git commit -m "feat(debug): complete DebugPass implementation with all primitives

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage:**
- [x] DebugRegistry class - Task 1
- [x] IDebugPrimitive interface - Task 1
- [x] DebugSettings struct - Task 1, Task 3
- [x] 6 primitives - Task 2
- [x] Renderer integration - Task 4
- [x] DebugPass execute rewrite - Task 6
- [x] ImGui panel - Task 7
- [x] Stats overlay - Task 2 (StatsOverlayPrimitive), Task 8

**2. Placeholder scan:**
- NormalOverlayPrimitive has "TODO" placeholder - noted as incomplete, requires GBuffer readback
- All other primitives have complete implementations

**3. Type consistency:**
- DebugVertex used consistently across all primitives
- DebugSettings fields match between shader_io.h and DebugRegistry.h
- PassContext.params.debugSettings matches RenderParams.debugSettings