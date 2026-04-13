#pragma once

#include "../DebugRegistry.h"
#include "../Renderer.h"
#include "../ShadowResources.h"
#include "../MeshPool.h"
#include "../../shaders/shader_io.h"
#include "../../rhi/RHITypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstring>

namespace demo {

// Helper: Write a vertex with position and color
inline void writeDebugVertex(DebugVertex* ptr, uint32_t index, const glm::vec3& pos, const glm::vec4& color)
{
    ptr[index].position[0] = pos.x;
    ptr[index].position[1] = pos.y;
    ptr[index].position[2] = pos.z;
    std::memcpy(ptr[index].color, &color, sizeof(float) * 4);
}

// Helper: Compute frustum corners from inverse view-projection matrix
inline std::array<glm::vec3, 8> computeFrustumCorners(const glm::mat4& invViewProj, float nearZ = 0.0f, float farZ = 1.0f)
{
    // NDC corners for Vulkan (Z in [0,1])
    const std::array<glm::vec4, 8> ndcCorners = {{
        // Near plane (Z=0)
        glm::vec4(-1.0f, -1.0f, nearZ, 1.0f),
        glm::vec4( 1.0f, -1.0f, nearZ, 1.0f),
        glm::vec4(-1.0f,  1.0f, nearZ, 1.0f),
        glm::vec4( 1.0f,  1.0f, nearZ, 1.0f),
        // Far plane (Z=1)
        glm::vec4(-1.0f, -1.0f, farZ, 1.0f),
        glm::vec4( 1.0f, -1.0f, farZ, 1.0f),
        glm::vec4(-1.0f,  1.0f, farZ, 1.0f),
        glm::vec4( 1.0f,  1.0f, farZ, 1.0f),
    }};

    std::array<glm::vec3, 8> worldCorners;
    for(size_t i = 0; i < 8; ++i)
    {
        glm::vec4 worldCorner = invViewProj * ndcCorners[i];
        if(std::fabs(worldCorner.w) > 0.001f)
            worldCorner /= worldCorner.w;
        worldCorners[i] = glm::vec3(worldCorner);
    }
    return worldCorners;
}

// Helper: Draw frustum wireframe (24 vertices for 12 lines)
inline uint32_t drawFrustumWireframe(DebugVertex* ptr, const std::array<glm::vec3, 8>& corners, const glm::vec4& color)
{
    // Line indices for frustum wireframe
    const std::array<int, 24> lineIndices = {{
        // Near plane edges (0-1-3-2-0)
        0, 1, 1, 3, 3, 2, 2, 0,
        // Far plane edges (4-5-7-6-4)
        4, 5, 5, 7, 7, 6, 6, 4,
        // Connecting edges
        0, 4, 1, 5, 2, 6, 3, 7,
    }};

    for(size_t i = 0; i < lineIndices.size(); ++i)
    {
        writeDebugVertex(ptr, i, corners[lineIndices[i]], color);
    }
    return static_cast<uint32_t>(lineIndices.size());
}

// Helper: Draw a circle in a plane (center, radius, segments, axis pair)
inline uint32_t drawCircle(DebugVertex* ptr, uint32_t startIdx, const glm::vec3& center, float radius,
                           uint32_t segments, const glm::vec3& axis1, const glm::vec3& axis2, const glm::vec4& color)
{
    for(uint32_t i = 0; i < segments; ++i)
    {
        const float angle0 = static_cast<float>(i) * 2.0f * 3.14159265f / static_cast<float>(segments);
        const float angle1 = static_cast<float>(i + 1) * 2.0f * 3.14159265f / static_cast<float>(segments);

        const glm::vec3 p0 = center + radius * (std::cos(angle0) * axis1 + std::sin(angle0) * axis2);
        const glm::vec3 p1 = center + radius * (std::cos(angle1) * axis1 + std::sin(angle1) * axis2);

        writeDebugVertex(ptr, startIdx + i * 2, p0, color);
        writeDebugVertex(ptr, startIdx + i * 2 + 1, p1, color);
    }
    return segments * 2;
}

// Helper: Draw axis cross at position (RGB colors)
inline uint32_t drawAxisCross(DebugVertex* ptr, uint32_t startIdx, const glm::vec3& pos, float length)
{
    // X axis (red)
    writeDebugVertex(ptr, startIdx + 0, pos, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    writeDebugVertex(ptr, startIdx + 1, pos + glm::vec3(length, 0.0f, 0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    // Y axis (green)
    writeDebugVertex(ptr, startIdx + 2, pos, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    writeDebugVertex(ptr, startIdx + 3, pos + glm::vec3(0.0f, length, 0.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
    // Z axis (blue)
    writeDebugVertex(ptr, startIdx + 4, pos, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    writeDebugVertex(ptr, startIdx + 5, pos + glm::vec3(0.0f, 0.0f, length), glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
    return 6;
}

// Helper: Draw arrow (line + arrowhead)
inline uint32_t drawArrow(DebugVertex* ptr, uint32_t startIdx, const glm::vec3& start, const glm::vec3& end,
                          const glm::vec4& color, float arrowHeadSize = 0.1f)
{
    // Main line
    writeDebugVertex(ptr, startIdx + 0, start, color);
    writeDebugVertex(ptr, startIdx + 1, end, color);

    // Arrowhead (simple perpendicular lines)
    const glm::vec3 dir = glm::normalize(end - start);
    glm::vec3 perp1, perp2;

    // Find perpendicular vectors
    if(std::abs(dir.y) < 0.99f)
    {
        perp1 = glm::normalize(glm::cross(dir, glm::vec3(0.0f, 1.0f, 0.0f)));
        perp2 = glm::normalize(glm::cross(dir, perp1));
    }
    else
    {
        perp1 = glm::normalize(glm::cross(dir, glm::vec3(0.0f, 0.0f, 1.0f)));
        perp2 = glm::normalize(glm::cross(dir, perp1));
    }

    const glm::vec3 arrowBase = end - dir * arrowHeadSize;
    writeDebugVertex(ptr, startIdx + 2, arrowBase + perp1 * arrowHeadSize * 0.5f, color);
    writeDebugVertex(ptr, startIdx + 3, end, color);
    writeDebugVertex(ptr, startIdx + 4, arrowBase - perp1 * arrowHeadSize * 0.5f, color);
    writeDebugVertex(ptr, startIdx + 5, end, color);

    return 6;
}

//------------------------------------------------------------------------------
// BoundingBoxPrimitive - Yellow wireframe boxes for mesh AABBs
//------------------------------------------------------------------------------
class BoundingBoxPrimitive : public IDebugPrimitive
{
public:
    bool isEnabled(const DebugSettings& settings) const override
    {
        return settings.showBounds;
    }

    uint32_t collectData(const PassContext& context, DebugVertex* vertexPtr) const override
    {
        if(!context.gltfModel || !context.params)
            return 0;

        Renderer* renderer = nullptr;  // Need renderer for MeshPool access
        // Note: This requires Renderer reference to access MeshPool
        // For now, return placeholder - needs mesh bounds in MeshRecord

        // Yellow color for bounds
        const glm::vec4 yellowColor(1.0f, 1.0f, 0.0f, 1.0f);

        uint32_t vertexCount = 0;

        // TODO: Iterate over meshes and draw AABB
        // Requires mesh bounds (min/max) to be stored in MeshRecord
        // Current implementation: MeshRecord doesn't store bounds
        // Future: Add bounds computation during mesh upload

        // Placeholder: Draw a unit box at origin to show the primitive works
        if(context.params->gltfModel)
        {
            // Draw one debug box at (0,0,0) for testing
            const std::array<glm::vec3, 8> boxCorners = {{
                glm::vec3(-0.5f, -0.5f, -0.5f),
                glm::vec3( 0.5f, -0.5f, -0.5f),
                glm::vec3(-0.5f,  0.5f, -0.5f),
                glm::vec3( 0.5f,  0.5f, -0.5f),
                glm::vec3(-0.5f, -0.5f,  0.5f),
                glm::vec3( 0.5f, -0.5f,  0.5f),
                glm::vec3(-0.5f,  0.5f,  0.5f),
                glm::vec3( 0.5f,  0.5f,  0.5f),
            }};
            vertexCount = drawFrustumWireframe(vertexPtr, boxCorners, yellowColor);
        }

        return vertexCount;
    }

    const char* getName() const override { return "BoundingBox"; }
};

//------------------------------------------------------------------------------
// BoundingSpherePrimitive - Cyan circles for mesh bounding spheres
//------------------------------------------------------------------------------
class BoundingSpherePrimitive : public IDebugPrimitive
{
public:
    bool isEnabled(const DebugSettings& settings) const override
    {
        return settings.showBoundingSpheres;
    }

    uint32_t collectData(const PassContext& context, DebugVertex* vertexPtr) const override
    {
        if(!context.gltfModel || !context.params)
            return 0;

        // Cyan color for bounding spheres
        const glm::vec4 cyanColor(0.0f, 1.0f, 1.0f, 1.0f);
        const uint32_t segments = 32;
        const float radius = 1.0f;  // Placeholder radius

        uint32_t vertexCount = 0;

        // TODO: Iterate over meshes and draw bounding spheres
        // Requires mesh bounds center and radius
        // Current implementation: MeshRecord doesn't store bounds

        // Placeholder: Draw a sphere at origin for testing
        // XZ plane circle
        vertexCount += drawCircle(vertexPtr, vertexCount, glm::vec3(0.0f), radius, segments,
                                   glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), cyanColor);
        // XY plane circle
        vertexCount += drawCircle(vertexPtr, vertexCount, glm::vec3(0.0f), radius, segments,
                                   glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), cyanColor);
        // YZ plane circle
        vertexCount += drawCircle(vertexPtr, vertexCount, glm::vec3(0.0f), radius, segments,
                                   glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), cyanColor);

        return vertexCount;
    }

    const char* getName() const override { return "BoundingSphere"; }
};

//------------------------------------------------------------------------------
// LightVisualizerPrimitive - Arrow for light direction, RGB cross at position
//------------------------------------------------------------------------------
class LightVisualizerPrimitive : public IDebugPrimitive
{
public:
    bool isEnabled(const DebugSettings& settings) const override
    {
        return settings.showLights;
    }

    uint32_t collectData(const PassContext& context, DebugVertex* vertexPtr) const override
    {
        if(!context.params)
            return 0;

        // Get light direction from RenderParams
        const glm::vec3 lightDir = context.params->lightDirection;
        // Note: lightVisualizationScale is in DebugSettings, not accessible via collectData
        // Future: Add DebugSettings* to PassContext or pass via RenderParams
        const float lightScale = 1.0f;  // Default scale

        // Light position (arbitrary, at some distance from scene center)
        // The light direction points FROM light TO scene, so light is in opposite direction
        const glm::vec3 sceneCenter(0.0f, 0.0f, 0.0f);  // Default center
        const glm::vec3 lightPos = sceneCenter - lightDir * 10.0f * lightScale;

        uint32_t vertexCount = 0;

        // Draw light direction arrow (white)
        const glm::vec3 arrowEnd = lightPos + lightDir * 2.0f * lightScale;
        vertexCount += drawArrow(vertexPtr, vertexCount, lightPos, arrowEnd,
                                 glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), 0.2f * lightScale);

        // Draw RGB axis cross at light position
        vertexCount += drawAxisCross(vertexPtr, vertexCount, lightPos, 0.5f * lightScale);

        return vertexCount;
    }

    const char* getName() const override { return "LightVisualizer"; }
};

//------------------------------------------------------------------------------
// FrustumWireframePrimitive - Camera frustum + shadow cascade frustums
//------------------------------------------------------------------------------
class FrustumWireframePrimitive : public IDebugPrimitive
{
public:
    bool isEnabled(const DebugSettings& settings) const override
    {
        return settings.showFrustum;
    }

    uint32_t collectData(const PassContext& context, DebugVertex* vertexPtr) const override
    {
        if(!context.params || !context.params->cameraUniforms)
            return 0;

        uint32_t vertexCount = 0;

        // Camera frustum (magenta)
        if(context.params->cameraUniforms)
        {
            const shaderio::CameraUniforms& camera = *context.params->cameraUniforms;
            const glm::mat4 invViewProj = glm::inverse(camera.viewProjection);

            const std::array<glm::vec3, 8> cameraCorners = computeFrustumCorners(invViewProj);
            const glm::vec4 magentaColor(1.0f, 0.0f, 1.0f, 1.0f);
            vertexCount += drawFrustumWireframe(vertexPtr, vertexCount, cameraCorners, magentaColor);
        }

        // Shadow cascade frustums (color-coded)
        // Note: Current ShadowResources has single frustum (no CSM cascades)
        // Future: Add cascade support with cascadeOffsets/cascadeScales
        if(context.params->cameraUniforms)
        {
            // Use ShadowResources lightViewProjectionMatrix for shadow frustum
            // This requires Renderer access - placeholder for now

            // Shadow frustum colors per cascade:
            // 0=red, 1=green, 2=blue, 3=cyan
            const std::array<glm::vec4, 4> cascadeColors = {{
                glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),  // Red
                glm::vec4(0.0f, 1.0f, 0.0f, 1.0f),  // Green
                glm::vec4(0.0f, 0.0f, 1.0f, 1.0f),  // Blue
                glm::vec4(0.0f, 1.0f, 1.0f, 1.0f),  // Cyan
            }};

            // TODO: Draw shadow cascade frustums when CSM is implemented
            // Currently single shadow frustum - draw in orange to distinguish
            // Placeholder for single shadow frustum visualization
        }

        return vertexCount;
    }

    const char* getName() const override { return "FrustumWireframe"; }
};

//------------------------------------------------------------------------------
// NormalOverlayPrimitive - Placeholder (requires GBuffer readback)
//------------------------------------------------------------------------------
class NormalOverlayPrimitive : public IDebugPrimitive
{
public:
    bool isEnabled(const DebugSettings& settings) const override
    {
        return settings.showNormals;
    }

    uint32_t collectData(const PassContext& context, DebugVertex* vertexPtr) const override
    {
        // TODO: Requires GBuffer normal texture readback
        // This would need:
        // 1. Read GBuffer normal texture
        // 2. Sample normals at specified density
        // 3. Draw lines showing normal direction at each sample point

        // Placeholder: Return 0 vertices
        // Future implementation would read GBuffer and draw normal vectors
        return 0;
    }

    const char* getName() const override { return "NormalOverlay"; }
};

//------------------------------------------------------------------------------
// StatsOverlayPrimitive - ImGui stats window (no geometry)
//------------------------------------------------------------------------------
class StatsOverlayPrimitive : public IDebugPrimitive
{
public:
    bool isEnabled(const DebugSettings& settings) const override
    {
        return settings.showStats;
    }

    uint32_t collectData(const PassContext& context, DebugVertex* vertexPtr) const override
    {
        // Stats overlay draws ImGui directly, no geometry
        return 0;
    }

    const char* getName() const override { return "StatsOverlay"; }

    bool hasImGuiOverlay() const override { return true; }

    void drawImGui(const PassContext& context) const override
    {
        if(!context.params)
            return;

        // Stats window at (10, 10)
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(200.0f, 150.0f), ImGuiCond_FirstUseEver);

        if(ImGui::Begin("Stats", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            // FPS and frame time (using ImGui's built-in metrics)
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("Frame: %.3f ms", 1000.0f / ImGui::GetIO().Framerate);

            // Mesh/Material/Texture counts from glTF model
            if(context.params->gltfModel)
            {
                const GltfUploadResult* model = context.params->gltfModel;
                ImGui::Text("Meshes: %zu", model->meshes.size());
                ImGui::Text("Materials: %zu", model->materials.size());
                ImGui::Text("Textures: %zu", model->textures.size());
            }
            else
            {
                ImGui::Text("Meshes: 0");
                ImGui::Text("Materials: 0");
                ImGui::Text("Textures: 0");
            }

            // Camera position
            if(context.params->cameraUniforms)
            {
                const glm::vec3& camPos = context.params->cameraUniforms->cameraPosition;
                ImGui::Text("Camera: (%.1f, %.1f, %.1f)", camPos.x, camPos.y, camPos.z);
            }

            ImGui::End();
        }
    }
};

//------------------------------------------------------------------------------
// Register all debug primitives with the registry
//------------------------------------------------------------------------------
inline void registerDebugPrimitives(DebugRegistry& registry)
{
    registry.registerPrimitive("BoundingBox", []() -> std::unique_ptr<IDebugPrimitive> {
        return std::make_unique<BoundingBoxPrimitive>();
    });

    registry.registerPrimitive("BoundingSphere", []() -> std::unique_ptr<IDebugPrimitive> {
        return std::make_unique<BoundingSpherePrimitive>();
    });

    registry.registerPrimitive("LightVisualizer", []() -> std::unique_ptr<IDebugPrimitive> {
        return std::make_unique<LightVisualizerPrimitive>();
    });

    registry.registerPrimitive("FrustumWireframe", []() -> std::unique_ptr<IDebugPrimitive> {
        return std::make_unique<FrustumWireframePrimitive>();
    });

    registry.registerPrimitive("NormalOverlay", []() -> std::unique_ptr<IDebugPrimitive> {
        return std::make_unique<NormalOverlayPrimitive>();
    });

    registry.registerPrimitive("StatsOverlay", []() -> std::unique_ptr<IDebugPrimitive> {
        return std::make_unique<StatsOverlayPrimitive>();
    });
}

}  // namespace demo