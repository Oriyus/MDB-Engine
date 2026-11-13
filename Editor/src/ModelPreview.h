#pragma once
#include "Renderer/Camera.h"
#include "Scene/Scene.h"

#include <entt/entt.hpp>
#include <memory>
#include <string>

namespace Engine { class SceneRenderer; class ResourceLibrary; class Model; }

// The Inspector's preview of a model file selected in the Assets panel (glTF, GLB, FBX, OBJ ...): its own small
// scene (the model scaled to 2 m and standing on a ground plane, a sun, the procedural sky) drawn by its own
// SceneRenderer, so it never touches the level being edited. Drag to orbit, wheel to zoom, double-click to reset;
// animated models play a clip.
//
// Render (outside the ImGui frame, from OnUpdate) draws only if Draw ran the frame before, so a hidden preview
// costs nothing. The renderer is made the first time a model is shown.
class ModelPreview {
public:
    ModelPreview(std::string engineDir, std::string assetDir);
    ~ModelPreview();

    // "" = nothing. The model loads at the next Render.
    void SetAsset(const std::string& path);
    const std::string& Asset() const { return m_Path; }
    bool Active() const { return !m_Path.empty(); }

    void Render(float dt, Engine::ResourceLibrary& resources);
    // The Inspector section: header, picture, info. height = the space it gets.
    void Draw(float height);
    // How tall Draw would like to be for a panel this wide
    float PreferredHeight(float width) const;

    void ReloadShaders();

private:
    void Build(Engine::ResourceLibrary& resources);
    void ResetView();
    void UpdateCamera();

    std::string m_EngineDir, m_AssetDir;
    std::string m_Path;                       // the asset shown
    std::string m_Built;                      // the asset the scene was built for
    bool m_Failed = false;

    Engine::Scene m_Scene;
    std::unique_ptr<Engine::SceneRenderer> m_Renderer;
    Engine::Camera m_Camera{ 35.0f, 1.0f, 0.02f, 200.0f };
    std::shared_ptr<Engine::Model> m_Model;
    entt::entity m_ModelEntity = entt::null, m_Ground = entt::null;

    // Framing: the model's centre and radius after it is scaled to 2 m; the orbit around them
    glm::vec3 m_Center{ 0.0f, 1.0f, 0.0f };
    float m_Radius = 1.0f;
    float m_Yaw = 35.0f, m_Pitch = 18.0f, m_Distance = 4.0f;   // degrees, metres
    glm::vec3 m_FileSize{ 0.0f };             // bounds in file units

    uint32_t m_Width = 0, m_Height = 0;       // the picture's size last frame (pixels)
    bool m_Drawn = false;                     // Draw ran since the last Render
    bool m_ShowGround = true, m_Spin = false, m_Playing = true;
    int m_Clip = 0;
};
