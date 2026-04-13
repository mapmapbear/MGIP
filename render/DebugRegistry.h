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