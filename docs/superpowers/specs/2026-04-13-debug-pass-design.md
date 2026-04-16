# DebugPass Design Specification

**Date:** 2026-04-13
**Status:** Approved
**Scope:** Replace ShadowDebugPass with unified DebugPass for comprehensive debug visualization

---

## Overview

Replace the current ShadowDebugPass (which only draws shadow frustum wireframes) with a unified DebugPass that provides comprehensive debug visualization tools for geometry, lights, cameras, and performance stats.

---

## Architecture

### Component Structure

```
┌──────────────────────────────────────────────────────────────────┐
│                         DebugPass                                  │
│                                                                    │
│  Responsibilities:                                                │
│  - Orchestrates all debug primitives                              │
│  - Manages transient buffer allocation                            │
│  - Collects enabled primitives from registry                      │
│  - Renders line/point geometry overlays                           │
│  - Draws ImGui text/stats overlay                                 │
│                                                                    │
│  ┌──────────────────┐    ┌─────────────────────────────────────┐ │
│  │  DebugRegistry   │    │         DebugSettings               │ │
│  │                  │◄───│  (from ImGui/RenderParams)          │ │
│  │  Primitive map:  │    │                                     │ │
│  │   "bounds" →     │    │  showBounds: bool                   │ │
│  │    factory()     │    │  showBoundingSpheres: bool          │ │
│  │   "spheres" →    │    │  showLights: bool                   │ │
│  │    factory()     │    │  showFrustum: bool                  │ │
│  │   "lights" →     │    │  showNormals: bool                  │ │
│  │    factory()     │    │  showStats: bool                    │ │
│  │   "frustum" →    │    │                                     │ │
│  │    factory()     │    │  Visualization options:             │ │
│  │   "normals" →    │    │  - lightVisualizationScale          │ │
│  │    factory()     │    │  - boundsScale                      │ │
│  │   "stats" →      │    │  - normalVisualizationLength        │ │
│  │    factory()     │    │  - cascadeIndex, selectedLightIndex │ │
│  └──────────────────┘    └─────────────────────────────────────┘ │
│                                                                    │
│  execute(context):                                                │
│    1. Query registry for enabled primitives                       │
│    2. Collect geometry into transient buffer                      │
│    3. Draw line/point primitives on lit scene                     │
│    4. Draw ImGui overlay (FPS, stats, labels)                     │
└──────────────────────────────────────────────────────────────────┘
```

### Pipeline Position

```
ShadowPass → GBufferPass → LightCullingPass → LightPass → DebugPass → ImguiPass → PresentPass
```

DebugPass runs **after LightPass** to overlay on the lit scene, and **before ImguiPass** so ImGui UI draws on top.

---

## Primitive Interface

### IDebugPrimitive

```cpp
class IDebugPrimitive {
public:
    virtual ~IDebugPrimitive() = default;
    
    // Check if primitive is enabled based on settings
    virtual bool isEnabled(const DebugSettings& settings) const = 0;
    
    // Collect data from scene/resources into transient buffer
    // Returns vertex count, writes to cpuPtr
    virtual uint32_t collectData(
        const PassContext& context,
        void* cpuPtr,
        const DebugVertexFormat format
    ) const = 0;
    
    // Get primitive topology (lines, points, triangles)
    virtual rhi::PrimitiveTopology getTopology() const = 0;
    
    // Optional: override pipeline
    virtual PipelineHandle getOverridePipeline() const { return {}; }
    
    // Name for registry and ImGui labels
    virtual const char* getName() const = 0;
};
```

### DebugVertexFormat (28 bytes, packed)

```cpp
struct DebugVertex {
    float position[3];   // 12 bytes - world space position
    float color[4];      // 16 bytes - RGBA color
};
```

### Primitive Topologies

| Primitive | Topology | Vertices |
|-----------|----------|----------|
| BoundingBox | Lines | 24 (12 edges) |
| BoundingSphere | Lines | 32-64 (circle segments) |
| LightDirection | Lines | 2-6 (arrow + cross) |
| FrustumWireframe | Lines | 24 (12 edges) |
| NormalVisualization | Lines | Per-pixel pairs |
| StatsOverlay | None | ImGui text |

---

## Primitive Implementations

### 1. BoundingBoxPrimitive

- Draws mesh bounding boxes from scene geometry
- Axis-colored edges (X=red, Y=yellow, Z=blue) or uniform yellow
- Settings: `showBounds`, `boundsScale`, `boundsForAllMeshes`

### 2. BoundingSpherePrimitive

- Draws bounding spheres for lights and scene objects
- 32-64 segment circle wireframes in XZ, XY, YZ planes
- Settings: `showBoundingSpheres`, `boundsScale`

### 3. LightVisualizerPrimitive

- Draws light positions, directions, influence ranges
- Directional: arrow pointing toward scene
- Point: sphere outline + center cross
- Spot: cone wireframe + direction arrow
- Settings: `showLights`, `lightVisualizationScale`, `selectedLightIndex`

### 4. FrustumWireframePrimitive

- Draws camera frustum and shadow cascade frustums
- Camera frustum from invViewProj
- Shadow cascade frustums color-coded (cascade 0=red, 1=green, 2=blue, 3=cyan)
- Settings: `showFrustum`, `showCameraFrustum`, `showShadowCascades`, `cascadeIndex`

### 5. NormalOverlayPrimitive

- Draws normal vectors as lines (GBuffer debug mode)
- Reads GBuffer normal texture
- For each pixel (or sparse grid): position → position + normal * scale
- Settings: `showNormals`, `normalVisualizationLength`, `normalVisualizationDensity`

### 6. StatsOverlayPrimitive

- ImGui-based FPS/stats display
- Fixed position overlay (top-left corner, 10,10)
- Semi-transparent background (alpha=0.35)
- Displays: FPS, frame time, mesh/material/texture counts, camera position
- Settings: `showStats` (default: true)

---

## DebugSettings

```cpp
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
```

---

## DebugRegistry

```cpp
class DebugRegistry {
public:
    using PrimitiveFactory = std::function<std::unique_ptr<IDebugPrimitive>()>;
    
    void registerPrimitive(const std::string& name, PrimitiveFactory factory);
    
    std::vector<std::unique_ptr<IDebugPrimitive>> 
        createEnabledPrimitives(const DebugSettings& settings) const;
    
    const std::map<std::string, PrimitiveFactory>& getAllFactories() const;
    
private:
    std::map<std::string, PrimitiveFactory> m_factories;
};
```

**Registration in Renderer::init():**

```cpp
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

---

## ImGui Panel

Integrated into existing "Settings" window as collapsible section.

**Panel Features:**

- Section headers: Geometry, Lights, Frustum, Normals
- Checkbox toggles for each primitive type
- Slider controls for visualization parameters (scale, length, density)
- Combo selectors for light/cascade index filtering

**Stats Overlay:**

- Fixed position: (10, 10) top-left
- No decoration, always auto-resize
- Semi-transparent background (alpha 0.35)
- Content: FPS, frame time (ms), mesh count, material count, texture count, camera position

---

## Execution Flow

```cpp
void DebugPass::execute(const PassContext& context) const {
    // 1. Early exit if nothing enabled
    // 2. Allocate transient vertex buffer (~10KB worst case)
    // 3. Collect geometry from all enabled primitives
    // 4. Flush and bind vertex buffer
    // 5. Begin overlay render pass (LoadOp::load for color/depth)
    // 6. Bind debug line pipeline
    // 7. Bind camera descriptor set
    // 8. Draw all lines
    // 9. End render pass
    // 10. Draw ImGui stats overlay (if enabled)
}
```

**Buffer Allocation:**

- Max debug vertices: 4096
- Vertex size: 28 bytes (position + color)
- Total buffer: ~114KB worst case
- Alignment: 16 bytes

---

## Integration Points

### RenderParams

```cpp
struct RenderParams {
    // ... existing fields ...
    DebugSettings debugSettings;  // New field
};
```

### MinimalLatestApp

```cpp
// In run() loop:
frameParams.debugSettings = m_debugSettings;

// In Settings ImGui window:
drawDebugPanel(m_debugSettings, m_renderer->getDebugRegistry());
```

---

## File Structure

```
render/
├── passes/
│   ├── DebugPass.h          // Renamed from ShadowDebugPass.h
│   ├── DebugPass.cpp        // Renamed from ShadowDebugPass.cpp
│   └── DebugPrimitives.h    // New: all primitive implementations
├── DebugRegistry.h          // New: registry class
├── Renderer.h               // Add: m_debugRegistry, getDebugRegistry()
├── Renderer.cpp             // Add: initDebugPrimitives()
└── PassContext.h            // (unchanged, uses RenderParams)

shaders/
└── shader.debug.slang       // Existing, unchanged (simple line shader)
```

---

## Migration Plan

1. Rename `ShadowDebugPass.h/cpp` → `DebugPass.h/cpp`
2. Create `DebugPrimitives.h` with 6 primitive classes
3. Create `DebugRegistry.h`
4. Add `DebugSettings` to `RenderParams` (in shader_io.h or separate header)
5. Update Renderer to initialize registry and pass settings
6. Update MinimalLatestApp to use new debug panel
7. Remove old shadow-specific debug code from ShadowDebugPass

---

## Success Criteria

1. All 6 primitives render correctly when enabled
2. ImGui panel toggles work without frame lag
3. Stats overlay displays accurate FPS and counts
4. Debug geometry overlays on lit scene without depth fighting
5. Extensible: adding new primitive requires only registration call