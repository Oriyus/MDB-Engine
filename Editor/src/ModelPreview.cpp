#include "ModelPreview.h"

#include "Renderer/SceneRenderer.h"
#include "Renderer/Environment.h"
#include "Renderer/Model.h"
#include "Runtime/ResourceLibrary.h"
#include "Scene/Entity.h"
#include "Core/Log.h"

#include <imgui.h>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace Engine;

ModelPreview::ModelPreview(std::string engineDir, std::string assetDir)
    : m_EngineDir(std::move(engineDir)), m_AssetDir(std::move(assetDir)) {}

ModelPreview::~ModelPreview() = default;

void ModelPreview::SetAsset(const std::string& path) {
    if (path == m_Path) return;
    m_Path = path;
    m_Failed = false;
}

void ModelPreview::ReloadShaders() { if (m_Renderer) m_Renderer->ReloadShaders(); }

float ModelPreview::PreferredHeight(float width) const {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float line = ImGui::GetFrameHeightWithSpacing();
    const float picture = std::clamp(width * 0.8f, 120.0f, 320.0f);
    // header + picture + options row + info lines
    return st.ItemSpacing.y + line + picture + st.ItemSpacing.y + line + ImGui::GetTextLineHeightWithSpacing() * 3.0f;
}

// ---------------------------------------------------------------- the scene
void ModelPreview::Build(ResourceLibrary& resources) {
    m_Built = m_Path;
    m_Scene.Clear();
    m_ModelEntity = m_Ground = entt::null;
    m_Model = resources.LoadModel(m_Path);
    m_Failed = !m_Model || !m_Model->IsValid();
    m_Clip = 0;
    m_Playing = true;
    if (m_Failed) { m_Model.reset(); return; }

    // Sun from the upper front left, the same kind as the example levels
    {
        Entity sun = m_Scene.CreateEntity("Sun");
        sun.Get<TransformComponent>().Rotation = glm::vec3(-0.75f, 0.6f, 0.0f);
        sun.Add<DirectionalLightComponent>().Intensity = 1.6f;
    }
    {
        Entity g = m_Scene.CreateEntity("Ground");           // 12 x 12 m
        auto& mr = g.Add<MeshRendererComponent>();
        mr.Geometry = resources.Plane();
        mr.Color = glm::vec3(0.42f, 0.43f, 0.45f);
        mr.Roughness = 0.9f;
        m_Ground = g.GetHandle();
    }

    // The model: 2 m across at its largest side, standing on the origin (as the editor places it), own materials
    Entity e = m_Scene.CreateEntity("Model");
    auto& mr = e.Add<MeshRendererComponent>();
    mr.Geometry = m_Model;
    mr.LocalOffset = m_Model->GetGroundedMatrix(2.0f);
    mr.UseModelMaterials = m_Model->HasTexturedMaterials();
    if (m_Model->HasVertexWind()) mr.Wind = 1.0f;
    if (m_Model->IsSkinned() && !m_Model->AnimationNames().empty()) {
        auto& an = e.Add<AnimatorComponent>();
        an.Clip = m_Model->AnimationNames().front();
        an.BlendTime = 0.0f;
    }
    m_ModelEntity = e.GetHandle();

    // Framing: the scaled bounds
    glm::vec3 mn, mx;
    m_Model->GetLocalBounds(mn, mx);
    m_FileSize = mx - mn;
    glm::vec3 wmn(1e30f), wmx(-1e30f);
    for (int i = 0; i < 8; i++) {
        const glm::vec3 c((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
        const glm::vec3 w = glm::vec3(mr.LocalOffset * glm::vec4(c, 1.0f));
        wmn = glm::min(wmn, w);
        wmx = glm::max(wmx, w);
    }
    m_Center = (wmn + wmx) * 0.5f;
    m_Radius = std::max(0.5f * glm::length(wmx - wmn), 0.05f);
    ResetView();

    if (m_Renderer) m_Renderer->ResetTemporalHistory();
}

void ModelPreview::ResetView() {
    m_Yaw = 35.0f;
    m_Pitch = 18.0f;
    // far enough for the bounding sphere to fit the vertical field of view, with a margin
    m_Distance = m_Radius / std::sin(glm::radians(m_Camera.GetFov() * 0.5f)) * 1.05f;
}

void ModelPreview::UpdateCamera() {
    m_Pitch = std::clamp(m_Pitch, -85.0f, 85.0f);
    m_Distance = std::clamp(m_Distance, m_Radius * 0.3f, m_Radius * 30.0f);
    const float yaw = glm::radians(m_Yaw), pitch = glm::radians(m_Pitch);
    const glm::vec3 dir(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    const glm::vec3 pos = m_Center + dir * m_Distance;
    m_Camera.SetLens(m_Camera.GetFov(), std::max(0.01f, m_Distance * 0.02f), m_Distance + m_Radius * 40.0f);
    m_Camera.SetAspect(m_Height ? (float)m_Width / (float)m_Height : 1.0f);
    m_Camera.SetPose(pos, glm::quatLookAt(glm::normalize(m_Center - pos), glm::vec3(0.0f, 1.0f, 0.0f)));
}

// ---------------------------------------------------------------- frame
void ModelPreview::Render(float dt, ResourceLibrary& resources) {
    const bool drawn = m_Drawn;
    m_Drawn = false;
    if (!drawn || m_Path.empty() || m_Width < 8 || m_Height < 8) return;

    if (!m_Renderer) {
        m_Renderer = std::make_unique<SceneRenderer>(m_EngineDir, m_AssetDir, m_Width, m_Height);
        auto& sh = m_Renderer->Shadows();
        sh.ShadowDistance = 12.0f;              // a 2 m model: the cascades close in
        sh.CascadeCount = 2;
        m_Renderer->GetEnvironment().Blur = 2.0f;
        m_Renderer->Post.Vignette = 0.25f;
    }
    if (m_Built != m_Path) Build(resources);
    if (m_Failed) return;

    if (m_Ground != entt::null) {
        auto& mr = m_Scene.Registry().get<MeshRendererComponent>(m_Ground);
        mr.Hidden = !m_ShowGround;
    }
    if (m_Spin) m_Yaw += dt * 20.0f;
    if (m_ModelEntity != entt::null && m_Scene.Registry().all_of<AnimatorComponent>(m_ModelEntity)) {
        auto& an = m_Scene.Registry().get<AnimatorComponent>(m_ModelEntity);
        const auto& clips = m_Model->AnimationNames();
        m_Clip = std::clamp(m_Clip, 0, (int)clips.size() - 1);
        an.Clip = clips[m_Clip];
        an.Playing = m_Playing;
    }
    m_Scene.UpdateAnimations(dt);
    UpdateCamera();
    m_Renderer->Render(m_Scene, m_Camera, m_Width, m_Height);
}

// ---------------------------------------------------------------- the Inspector section
void ModelPreview::Draw(float height) {
    if (m_Path.empty()) return;
    const float startY = ImGui::GetCursorPosY();
    const size_t slash = m_Path.find_last_of('/');
    const std::string name = slash == std::string::npos ? m_Path : m_Path.substr(slash + 1);

    ImGui::SeparatorText(("Preview: " + name).c_str());
    ImGui::SetItemTooltip("%s", m_Path.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
    if (ImGui::SmallButton("x##closepreview")) { SetAsset(""); return; }
    ImGui::SetItemTooltip("Close the preview (select a model in Assets to show it again)");

    const float lines = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 3.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float picH = std::max(64.0f, height - (ImGui::GetCursorPosY() - startY) - lines - ImGui::GetStyle().ItemSpacing.y);
    const ImVec2 size(avail, picH);

    if (m_Failed) {
        ImGui::Dummy(ImVec2(avail, 8.0f));
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Can't load %s", m_Path.c_str());
        return;
    }

    // The picture, and the mouse on it: drag orbits, right / middle drag zooms, wheel zooms, double-click resets
    m_Width = (uint32_t)size.x;
    m_Height = (uint32_t)size.y;
    m_Drawn = true;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##previewimage", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const bool ready = m_Renderer && m_Built == m_Path && m_Renderer->GetWidth() == m_Width && m_Renderer->GetHeight() == m_Height;
    if (ready) dl->AddImage((ImTextureID)(intptr_t)m_Renderer->GetOutput(), p0, p1, ImVec2(0, 1), ImVec2(1, 0));
    else {
        dl->AddRectFilled(p0, p1, IM_COL32(30, 31, 34, 255));
        const char* t = "Loading...";
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f), IM_COL32(150, 150, 150, 255), t);
    }
    dl->AddRect(p0, p1, IM_COL32(0, 0, 0, 90));

    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActive()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_Yaw -= io.MouseDelta.x * 0.4f;
            m_Pitch += io.MouseDelta.y * 0.4f;
        }
        else if (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle))
            m_Distance *= std::exp(io.MouseDelta.y * 0.006f);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);       // the wheel zooms instead of scrolling the panel
        if (io.MouseWheel != 0.0f) m_Distance *= std::pow(0.88f, io.MouseWheel);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ResetView();
        if (!ImGui::IsItemActive()) ImGui::SetTooltip("Drag to orbit, wheel or right-drag to zoom, double-click to reset");
    }

    // Options
    ImGui::Checkbox("Ground", &m_ShowGround);
    ImGui::SameLine();
    ImGui::Checkbox("Turn", &m_Spin);
    ImGui::SetItemTooltip("Turn the camera slowly round the model");
    if (m_Model && m_Model->IsSkinned() && !m_Model->AnimationNames().empty()) {
        const auto& clips = m_Model->AnimationNames();
        ImGui::SameLine();
        if (ImGui::SmallButton(m_Playing ? "||" : ">")) m_Playing = !m_Playing;
        ImGui::SetItemTooltip(m_Playing ? "Pause the animation" : "Play the animation");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x));
        m_Clip = std::clamp(m_Clip, 0, (int)clips.size() - 1);
        if (ImGui::BeginCombo("##clip", clips[m_Clip].c_str())) {
            for (int i = 0; i < (int)clips.size(); i++)
                if (ImGui::Selectable(clips[i].c_str(), i == m_Clip)) m_Clip = i;
            ImGui::EndCombo();
        }
    }

    // Facts about the file
    if (m_Model) {
        ImGui::TextDisabled("%u triangles, %u part%s%s", m_Model->TriangleCount(), m_Model->SubmeshCount(),
                            m_Model->SubmeshCount() == 1 ? "" : "s", m_Model->HasTexturedMaterials() ? ", textured" : "");
        ImGui::TextDisabled("Size %.2f x %.2f x %.2f%s", m_FileSize.x, m_FileSize.y, m_FileSize.z,
                            m_Model->NativeSize ? " m (kit piece)" : " (file units)");
        std::string extra;
        if (m_Model->IsSkinned()) extra += std::to_string(m_Model->BoneCount()) + " bones, " + std::to_string(m_Model->AnimationNames().size()) + " clips";
        if (m_Model->LodCount()) extra += (extra.empty() ? "" : ", ") + std::to_string(m_Model->LodCount() + 1) + " LOD levels";
        if (m_Model->HasCollisionMesh()) extra += (extra.empty() ? "" : ", ") + std::string("collision mesh");
        if (m_Model->HasVertexWind()) extra += (extra.empty() ? "" : ", ") + std::string("wind weights");
        ImGui::TextDisabled("%s", extra.empty() ? "Static model" : extra.c_str());
    }
}
