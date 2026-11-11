#pragma once
#include "Scene/Terrain.h"
#include <entt/entt.hpp>
#include <imgui.h>
#include <string>

namespace Engine { class Scene; class Camera; class ResourceLibrary; struct TerrainComponent; }

// The terrain tools: sculpt and paint brushes used in the Viewport, chosen in the Inspector's Terrain section, plus
// the whole-terrain operations there (generate, flatten, paint all, import a heightmap, resolution).
// Edit mode only. Every stroke and operation first copies the terrain's data (Engine::TerrainData), so the undo
// history's snapshots keep the old one; the change is recorded as one undo step when the mouse is let go.
class TerrainEditor {
public:
    enum class Tool { None, Raise, Smooth, Flatten, Noise, Paint };

    // The Terrain section of the Inspector for entity e (which has a TerrainComponent)
    void DrawInspector(Engine::Scene& scene, entt::entity e, Engine::ResourceLibrary& resources, const std::string& assetDir);

    // The brush in the Viewport for terrain entity e: a preview ring under the mouse and, while the left button is
    // held, the stroke. Returns true while a tool is chosen, so the Viewport neither selects nor shows the gizmo.
    bool Viewport(Engine::Scene& scene, entt::entity e, const Engine::Camera& camera, const ImVec2& origin, const ImVec2& size,
                  bool hovered, bool navigating, float dt);

    bool Active() const { return m_Tool != Tool::None; }
    bool Stroking() const { return m_Stroking; }
    void SetTool(Tool t) { m_Tool = t; m_Stroking = false; }
    void SetPaintLayer(int layer) { m_Brush.Layer = layer < 0 ? 0 : layer > 3 ? 3 : layer; }

    // Before the scene is saved: give every terrain a data file of its own ("Terrains/<scene>.terrain", then _2, _3...; also
    // when two share one after Duplicate) and write each one. sceneFile names the new files. newFiles (Save As): every
    // terrain gets a new file, so the two scenes never share one. False if one failed.
    static bool SaveTerrains(Engine::Scene& scene, Engine::ResourceLibrary& resources, const std::string& assetDir,
                             const std::string& sceneFile, bool newFiles = false);
    // A data path no terrain in the scene and no file uses yet, named after the scene
    static std::string NewDataPath(Engine::Scene& scene, const std::string& assetDir, const std::string& sceneFile);

private:
    // Copy the data before changing it (snapshots share the old one) and return the copy
    static Engine::TerrainData& Edit(Engine::TerrainComponent& t);
    bool PickPoint(Engine::Scene& scene, entt::entity e, const Engine::Camera& camera, const ImVec2& origin, const ImVec2& size,
                   glm::vec3& local) const;

    Tool m_Tool = Tool::None;
    Engine::TerrainBrush m_Brush;
    bool m_FlattenAtStart = true;      // Flatten: level to the height where the stroke starts
    bool m_Stroking = false;
    entt::entity m_StrokeEntity = entt::null;
    Engine::TerrainNoise m_Noise;
    float m_FillHeight = 0.0f;
    Engine::TerrainData::AutoPaintRules m_AutoPaint;
    char m_ImportPath[260] = "";
    float m_ImportLow = 0.0f, m_ImportRange = 20.0f;
    std::string m_Status;              // the last operation's result (import errors)
};
