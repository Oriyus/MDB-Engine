#pragma once
#include "Scene/Terrain.h"
#include <entt/entt.hpp>
#include <imgui.h>
#include <functional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine { class Scene; class Camera; class ResourceLibrary; struct TerrainComponent; }

// The terrain tools: sculpt and paint brushes used in the Viewport, chosen in the Inspector's Terrain section, plus
// the whole-terrain operations there (generate, flatten, paint all, import a heightmap, resolution).
// Edit mode only. Every stroke and operation first copies the terrain's data (Engine::TerrainData), so the undo
// history's snapshots keep the old one; the change is recorded as one undo step when the mouse is let go.
class TerrainEditor {
public:
    enum class Tool { None, Raise, Smooth, Flatten, Noise, Paint, Scatter };

    // Scatter: what to place. Path is a model or prefab under Assets; Weight is how often it is picked against the
    // others; Scale multiplies the instance's random scale (models are placed 2 m tall unless they are kit pieces).
    struct ScatterItem { std::string Path; float Weight = 1.0f; float Scale = 1.0f; };
    enum class ScatterCollider { Keep, None, Trunk, Mesh };
    struct ScatterSettings {
        std::vector<ScatterItem> Items;
        std::string Group = "Scatter";   // a top-level entity holding the instances (erase and re-snap use only it)
        float Density = 2.0f;            // instances per 100 m2
        float Spacing = 3.0f;            // metres between instances of the group, at least
        float ScaleMin = 0.8f, ScaleMax = 1.25f;
        bool  RandomYaw = true;
        float Align = 0.0f;              // 0 = upright, 1 = tilted to the ground's slope
        float Sink = 0.05f;              // metres into the ground (no gap on slopes)
        float MaxSlope = 35.0f;          // degrees
        bool  UseHeight = false;
        float MinHeight = -1000.0f, MaxHeight = 1000.0f;   // the terrain's space
        int   Layer = -1;                // only where this paint layer is at least LayerMin (-1: anywhere)
        float LayerMin = 0.5f;
        bool  Static = true;
        float Wind = 0.0f;               // set on every Mesh Renderer when above 0
        ScatterCollider Collider = ScatterCollider::Trunk;
        float TrunkWidth = 0.6f, TrunkHeight = 4.0f;
        uint32_t Seed = 1;
    };
    // Makes an instance of a model or prefab at the origin, unparented, without selecting it (main.cpp)
    using CreateAssetFn = std::function<entt::entity(const std::string& path)>;
    void SetCreateAsset(CreateAssetFn f) { m_CreateAsset = std::move(f); }
    ScatterSettings& Scatter() { return m_Scatter; }
    // Fill the whole terrain by the rules (up to Density, keeping Spacing to what is there); the number placed
    int ScatterFill(Engine::Scene& scene, entt::entity terrain);
    // Stand every instance of the group on the ground again (after sculpting); keeps their rotation. The number moved.
    int ScatterResnap(Engine::Scene& scene, entt::entity terrain);

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

    // ---- scatter
    entt::entity FindGroup(Engine::Scene& scene, entt::entity terrain, bool create);
    void BeginScatter(Engine::Scene& scene, entt::entity terrain);           // gathers the group's points
    bool TryPlace(Engine::Scene& scene, entt::entity terrain, const glm::vec2& p);   // terrain space X/Z
    bool SpacingFree(const glm::vec2& p) const;
    void AddPoint(const glm::vec2& p);
    int CountIn(const glm::vec2& c, float r) const;
    void ScatterStroke(Engine::Scene& scene, entt::entity terrain, const glm::vec2& centre, bool erase, float dt);
    void DrawScatterInspector(Engine::Scene& scene, entt::entity terrain, const std::string& assetDir);

    ScatterSettings m_Scatter;
    CreateAssetFn m_CreateAsset;
    std::mt19937 m_Rng{ 1u };
    float m_ScatterBudget = 0.0f;
    entt::entity m_ScatterGroup = entt::null;
    std::unordered_map<int64_t, std::vector<glm::vec2>> m_ScatterGrid;   // the group's points by Spacing cell
    float m_ScatterCell = 1.0f;

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
