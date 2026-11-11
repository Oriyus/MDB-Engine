#include "TerrainEditor.h"
#include "AssetBrowser.h"

#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Renderer/Camera.h"
#include "Renderer/Texture.h"
#include "Runtime/ResourceLibrary.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

using namespace Engine;

namespace {
    const char* kLayerNames[4] = { "Layer 1", "Layer 2", "Layer 3", "Layer 4" };

    std::string Stem(const std::string& sceneFile) {
        std::string s = std::filesystem::u8path(sceneFile).stem().string();
        if (s.empty()) s = "scene";
        for (char& c : s) if (c == ' ') c = '_';
        return s;
    }

    // A small toggle button (highlighted while on)
    bool Toggle(const char* label, bool on, const char* tip) {
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool pressed = ImGui::Button(label);
        if (on) ImGui::PopStyleColor();
        if (tip) ImGui::SetItemTooltip("%s", tip);
        return pressed;
    }
}

TerrainData& TerrainEditor::Edit(TerrainComponent& t) {
    t.Data = t.Data ? std::make_shared<TerrainData>(*t.Data) : std::make_shared<TerrainData>();
    return *t.Data;
}

std::string TerrainEditor::NewDataPath(Scene& scene, const std::string& assetDir, const std::string& sceneFile) {
    const std::string base = "Terrains/" + Stem(sceneFile);
    for (int n = 1; ; n++) {
        const std::string path = base + (n > 1 ? "_" + std::to_string(n) : std::string()) + ".terrain";
        bool used = false;
        for (auto [e, t] : scene.Registry().view<TerrainComponent>().each()) used = used || t.DataPath == path;
        std::error_code ec;
        if (!used && !std::filesystem::exists(std::filesystem::u8path(assetDir + path), ec)) return path;
    }
}

bool TerrainEditor::SaveTerrains(Scene& scene, ResourceLibrary& resources, const std::string& assetDir, const std::string& sceneFile,
                                 bool newFiles) {
    std::map<std::string, const TerrainData*> written;
    bool ok = true;
    for (auto [e, t] : scene.Registry().view<TerrainComponent>().each()) {
        if (!t.Data) continue;
        // A terrain needs its own file: one with none yet, or one sharing another's file with different contents
        // (Duplicate copies the path) gets a new one
        const auto it = written.find(t.DataPath);
        // Save As: a file that already exists belongs to the scene saved before; one not written yet is this scene's
        std::error_code ec;
        const bool otherScene = newFiles && !t.DataPath.empty() && it == written.end() &&
            std::filesystem::exists(std::filesystem::u8path(assetDir + t.DataPath), ec);
        if (otherScene || t.DataPath.empty() || (it != written.end() && it->second != t.Data.get()))
            t.DataPath = NewDataPath(scene, assetDir, sceneFile);
        if (written.count(t.DataPath)) continue;
        written[t.DataPath] = t.Data.get();
        ok = resources.SaveTerrain(t.DataPath, t.Data) && ok;
    }
    return ok;
}

bool TerrainEditor::PickPoint(Scene& scene, entt::entity e, const Camera& camera, const ImVec2& origin, const ImVec2& size,
                              glm::vec3& local) const {
    const auto& t = scene.Registry().get<TerrainComponent>(e);
    if (!t.Data || size.x <= 0.0f || size.y <= 0.0f) return false;
    const ImVec2 m = ImGui::GetMousePos();
    const float x = (m.x - origin.x) / size.x * 2.0f - 1.0f, y = 1.0f - (m.y - origin.y) / size.y * 2.0f;
    const glm::mat4 inv = glm::inverse(camera.GetViewProjection());
    glm::vec4 a = inv * glm::vec4(x, y, -1.0f, 1.0f), b = inv * glm::vec4(x, y, 1.0f, 1.0f);
    a /= a.w; b /= b.w;
    const glm::mat4 toLocal = glm::inverse(scene.GetWorldMatrix(e));
    const glm::vec3 o = glm::vec3(toLocal * a);
    const glm::vec3 d = glm::vec3(toLocal * glm::vec4(glm::vec3(b - a), 0.0f));   // near to far plane: t in 0..1
    float hit;
    if (!t.Data->Raycast(t.Size, o, d, 1.0f, hit)) return false;
    local = o + d * hit;
    return true;
}

bool TerrainEditor::Viewport(Scene& scene, entt::entity e, const Camera& camera, const ImVec2& origin, const ImVec2& size,
                             bool hovered, bool navigating, float dt) {
    auto& reg = scene.Registry();
    if (m_Tool == Tool::None || !reg.valid(e) || !reg.all_of<TerrainComponent>(e)) { m_Stroking = false; return false; }
    auto& tc = reg.get<TerrainComponent>(e);
    if (!tc.Data) tc.Data = std::make_shared<TerrainData>();
    ImGuiIO& io = ImGui::GetIO();

    // [ and ] resize the brush
    if (hovered && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) m_Brush.Radius = std::max(0.25f, m_Brush.Radius / 1.2f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) m_Brush.Radius = std::min(500.0f, m_Brush.Radius * 1.2f);
    }

    glm::vec3 local(0.0f);
    const bool hit = (hovered || m_Stroking) && !navigating && PickPoint(scene, e, camera, origin, size, local);

    if (!m_Stroking && hovered && !navigating && hit && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (m_Tool == Tool::Flatten && io.KeyCtrl) {
            m_Brush.Target = local.y;                    // Ctrl+click: pick the height to level to
        }
        else {
            Edit(tc);                                    // the undo history keeps the data as it was
            m_Stroking = true;
            m_StrokeEntity = e;
            if (m_Tool == Tool::Flatten && m_FlattenAtStart) m_Brush.Target = local.y;
        }
    }
    if (m_Stroking && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || m_StrokeEntity != e || navigating)) m_Stroking = false;

    if (m_Stroking && hit) {
        TerrainBrush b = m_Brush;
        switch (m_Tool) {
        case Tool::Raise:   b.Mode = io.KeyShift ? TerrainBrushMode::Lower : TerrainBrushMode::Raise; break;
        case Tool::Smooth:  b.Mode = TerrainBrushMode::Smooth; break;
        case Tool::Flatten: b.Mode = TerrainBrushMode::Flatten; break;
        case Tool::Noise:   b.Mode = TerrainBrushMode::Noise; break;
        case Tool::Paint:   b.Mode = TerrainBrushMode::Paint; break;
        default: break;
        }
        tc.Data->ApplyBrush(b, tc.Size, glm::vec2(local.x, local.z), std::clamp(dt, 0.0f, 0.1f));
    }

    // The brush: its edge and where its full strength ends, lying on the ground
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hit) {
        const glm::mat4 M = scene.GetWorldMatrix(e);
        const glm::mat4 clip = camera.GetViewProjection() * M;
        auto ring = [&](float r, ImU32 col, float thickness) {
            constexpr int kN = 72;
            ImVec2 pts[kN];
            for (int i = 0; i < kN; i++) {
                const float a = 6.2831853f * i / kN;
                const float x = local.x + std::cos(a) * r, z = local.z + std::sin(a) * r;
                float h = local.y;
                tc.Data->HeightAt(tc.Size, x, z, h);
                const glm::vec4 c = clip * glm::vec4(x, h + 0.03f, z, 1.0f);
                if (c.w <= 0.01f) return;                // part of it behind the camera
                pts[i] = ImVec2(origin.x + (c.x / c.w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - c.y / c.w * 0.5f) * size.y);
            }
            dl->AddPolyline(pts, kN, col, ImDrawFlags_Closed, thickness);
        };
        ImU32 col = IM_COL32(255, 200, 80, 230);
        if (m_Tool == Tool::Raise && io.KeyShift) col = IM_COL32(110, 170, 255, 230);
        else if (m_Tool == Tool::Smooth) col = IM_COL32(150, 255, 170, 230);
        else if (m_Tool == Tool::Flatten) col = IM_COL32(255, 140, 220, 230);
        else if (m_Tool == Tool::Paint) {
            const glm::vec3 c = tc.Layers[std::clamp(m_Brush.Layer, 0, 3)].Color;
            col = IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 255);
        }
        ring(m_Brush.Radius, IM_COL32(0, 0, 0, 120), 4.0f);   // a dark rim so it reads on bright ground
        ring(m_Brush.Radius, col, 2.0f);
        const float inner = m_Brush.Radius * (1.0f - std::clamp(m_Brush.Falloff, 0.0f, 1.0f));
        if (inner > 0.05f) ring(inner, (col & 0x00FFFFFFu) | (110u << 24), 1.0f);
        const glm::vec4 c = clip * glm::vec4(local, 1.0f);
        if (c.w > 0.01f) {
            const ImVec2 p(origin.x + (c.x / c.w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - c.y / c.w * 0.5f) * size.y);
            dl->AddCircleFilled(p, 2.5f, col);
        }
    }

    static const char* kHints[] = { "",
        "Raise: left drag   Shift: lower   [ ]: brush size",
        "Smooth: left drag   [ ]: brush size",
        "Flatten: left drag (to the height where you start)   Ctrl+click: pick the height   [ ]: brush size",
        "Noise: left drag roughens the ground   [ ]: brush size",
        "Paint: left drag paints the chosen layer   [ ]: brush size" };
    const char* hint = kHints[(int)m_Tool];
    const float w = ImGui::CalcTextSize(hint).x;
    dl->AddRectFilled(ImVec2(origin.x + 8.0f, origin.y + size.y - 30.0f), ImVec2(origin.x + 20.0f + w, origin.y + size.y - 8.0f),
                      IM_COL32(0, 0, 0, 140), 4.0f);
    dl->AddText(ImVec2(origin.x + 14.0f, origin.y + size.y - 27.0f), IM_COL32(255, 255, 255, 220), hint);
    return true;
}

void TerrainEditor::DrawInspector(Scene& scene, entt::entity entity, ResourceLibrary& resources, const std::string& assetDir) {
    auto& reg = scene.Registry();
    auto& t = reg.get<TerrainComponent>(entity);
    if (!t.Data) t.Data = std::make_shared<TerrainData>();
    ImGui::PushID("terrain");

    const uint32_t res = t.Data->Resolution();
    ImGui::TextDisabled("%u x %u samples, %.2f m apart, %u chunks", res, res, t.Size.x / (res - 1),
                        (uint32_t)t.Data->Chunks().size());
    ImGui::TextDisabled("Heights %.1f .. %.1f m", t.Data->MinHeight(), t.Data->MaxHeight());

    // ---- tools
    ImGui::SeparatorText("Tools");
    struct ToolButton { Tool T; const char* Label; const char* Tip; };
    static const ToolButton kTools[] = {
        { Tool::Raise,   "Raise",   "Raise the ground (Shift: lower it)" },
        { Tool::Smooth,  "Smooth",  "Even out bumps and sharp edges" },
        { Tool::Flatten, "Flatten", "Level the ground to one height (Ctrl+click picks it)" },
        { Tool::Noise,   "Noise",   "Roughen the ground with small bumps" },
        { Tool::Paint,   "Paint",   "Paint the chosen layer" },
    };
    for (const auto& b : kTools) {
        if (Toggle(b.Label, m_Tool == b.T, b.Tip)) SetTool(m_Tool == b.T ? Tool::None : b.T);
        ImGui::SameLine();
    }
    if (Toggle("Off", m_Tool == Tool::None, "No terrain tool: click to select, W / E / R for the gizmo")) SetTool(Tool::None);
    if (m_Tool != Tool::None) {
        ImGui::DragFloat("Radius", &m_Brush.Radius, 0.05f, 0.25f, 500.0f, "%.2f m");
        ImGui::SetItemTooltip("[ and ] in the Viewport change it too");
        ImGui::SliderFloat("Strength", &m_Brush.Strength, 0.01f, 1.0f);
        ImGui::SliderFloat("Falloff", &m_Brush.Falloff, 0.0f, 1.0f);
        ImGui::SetItemTooltip("0: full strength to the edge. 1: fading all the way from the centre.");
        if (m_Tool == Tool::Flatten) {
            ImGui::Checkbox("Height where the stroke starts", &m_FlattenAtStart);
            ImGui::BeginDisabled(m_FlattenAtStart);
            ImGui::DragFloat("Target height", &m_Brush.Target, 0.05f, -1000.0f, 1000.0f, "%.2f m");
            ImGui::EndDisabled();
        }
        if (m_Tool == Tool::Noise) {
            ImGui::DragFloat("Bump size", &m_Brush.NoiseScale, 0.05f, 0.2f, 100.0f, "%.1f m");
            int seed = (int)m_Brush.Seed;
            if (ImGui::DragInt("Seed##brush", &seed, 1.0f, 0, 1 << 30)) m_Brush.Seed = (uint32_t)std::max(seed, 0);
        }
        if (m_Tool == Tool::Paint) ImGui::Text("Painting: %s", kLayerNames[std::clamp(m_Brush.Layer, 0, 3)]);
        ImGui::TextDisabled("Edit mode only. Each stroke is one undo step.");
    }

    // ---- layers
    ImGui::SeparatorText("Layers");
    const auto registry = resources.Registry();
    for (int i = 0; i < TerrainComponent::MaxLayers; i++) {
        TerrainLayer& L = t.Layers[i];
        ImGui::PushID(i);
        if (ImGui::RadioButton("##paint", m_Tool == Tool::Paint && m_Brush.Layer == i)) { m_Brush.Layer = i; SetTool(Tool::Paint); }
        ImGui::SetItemTooltip("Paint with this layer");
        ImGui::SameLine();
        ImGui::ColorEdit3("##color", &L.Color.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SetItemTooltip("Tint (the colour itself without a texture)");
        ImGui::SameLine();
        std::string current = L.Texture ? registry.TextureName(L.Texture) : "";
        if (current.rfind("texture:", 0) == 0) current = current.substr(8);
        const std::string preview = current.empty() ? std::string(kLayerNames[i]) + " (colour only)" : current;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##texture", preview.c_str())) {
            if (ImGui::Selectable("(colour only)", !L.Texture)) L.Texture = nullptr;
            std::error_code ec;
            for (const auto& file : std::filesystem::recursive_directory_iterator(std::filesystem::u8path(assetDir) / "Textures", ec)) {
                if (!file.is_regular_file()) continue;
                const std::string rel = std::filesystem::relative(file.path(), std::filesystem::u8path(assetDir), ec).generic_string();
                if (AssetBrowser::KindOf(rel) != AssetBrowser::Kind::Texture) continue;
                if (ImGui::Selectable(rel.c_str(), current == rel))
                    if (auto tex = resources.LoadTexture(rel)) L.Texture = tex;
            }
            ImGui::EndCombo();
        }
        if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Texture }); !drop.empty())
            if (auto tex = resources.LoadTexture(drop)) L.Texture = tex;
        ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        ImGui::DragFloat("##tiling", &L.Tiling, 0.05f, 0.05f, 500.0f, "repeat %.1f m");
        ImGui::SetItemTooltip("The texture repeats every this many metres");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##rough", &L.Roughness, 0.04f, 1.0f, "roughness %.2f");
        ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::PopID();
    }
    ImGui::SliderFloat("Height blend", &t.Blend, 0.0f, 1.0f);
    ImGui::SetItemTooltip("Where layers meet, the brighter (higher) parts of a texture show through first:\n"
                          "stones poke through grass instead of a soft fade. 0: plain mixing.");
    ImGui::Checkbox("Cast shadows", &t.CastShadows);

    // ---- shape
    ImGui::SeparatorText("Shape");
    ImGui::DragFloat2("Size", &t.Size.x, 0.25f, 1.0f, 10000.0f, "%.1f m");
    ImGui::SetItemTooltip("Metres in X and Z, centred on the entity. Stretching keeps the samples.");
    t.Size = glm::max(t.Size, glm::vec2(1.0f));
    static const uint32_t kRes[] = { 33, 65, 129, 257, 513, 1025 };
    int current = 0;
    for (int i = 0; i < 6; i++) if (kRes[i] == res) current = i;
    char label[32];
    std::snprintf(label, sizeof(label), "%u", res);
    if (ImGui::BeginCombo("Resolution", label)) {
        for (int i = 0; i < 6; i++) {
            std::snprintf(label, sizeof(label), "%u x %u", kRes[i], kRes[i]);
            if (ImGui::Selectable(label, i == current) && kRes[i] != res) t.Data = t.Data->Resampled(kRes[i]);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Samples per side. Changing it resamples the heights and paint.");

    if (ImGui::TreeNode("Generate")) {
        ImGui::DragFloat("Height##gen", &m_Noise.Height, 0.1f, 0.0f, 2000.0f, "%.1f m");
        ImGui::SetItemTooltip("From the lowest to the highest point");
        ImGui::DragFloat("Hill size", &m_Noise.FeatureSize, 0.25f, 1.0f, 5000.0f, "%.1f m");
        ImGui::SliderInt("Detail", &m_Noise.Octaves, 1, 8);
        ImGui::SliderFloat("Roughness##gen", &m_Noise.Roughness, 0.1f, 0.9f);
        ImGui::SliderFloat("Ridges", &m_Noise.Ridges, 0.0f, 1.0f);
        ImGui::SetItemTooltip("0: rounded hills. 1: sharp mountain ridges.");
        int seed = (int)m_Noise.Seed;
        if (ImGui::DragInt("Seed##gen", &seed, 1.0f, 0, 1 << 30)) m_Noise.Seed = (uint32_t)std::max(seed, 0);
        if (ImGui::Button("Generate")) { m_Noise.Add = false; Edit(t).Generate(m_Noise, t.Size); }
        ImGui::SetItemTooltip("Replace the heights with fractal hills");
        ImGui::SameLine();
        if (ImGui::Button("Add to current")) { m_Noise.Add = true; Edit(t).Generate(m_Noise, t.Size); }
        ImGui::SameLine();
        if (ImGui::Button("New seed")) m_Noise.Seed = m_Noise.Seed * 1664525u + 1013904223u;
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Flatten and fill")) {
        ImGui::DragFloat("Height##fill", &m_FillHeight, 0.05f, -1000.0f, 1000.0f, "%.2f m");
        ImGui::SameLine();
        if (ImGui::Button("Flatten all")) Edit(t).Fill(m_FillHeight);
        for (int i = 0; i < TerrainComponent::MaxLayers; i++) {
            if (i) ImGui::SameLine();
            char b[32];
            std::snprintf(b, sizeof(b), "Paint all %d", i + 1);
            if (ImGui::Button(b)) Edit(t).FillPaint(i);
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Auto paint")) {
        // Paint the whole terrain from its shape: rock on slopes, sand low down, snow up high
        const char* names[] = { "(none)", "Layer 1", "Layer 2", "Layer 3", "Layer 4" };
        auto pick = [&](const char* label, int& layer) {
            int v = layer + 1;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
            if (ImGui::Combo(label, &v, names, 5)) layer = v - 1;
        };
        auto& r = m_AutoPaint;
        pick("Everywhere else", r.BaseLayer);
        pick("##steep", r.SteepLayer); ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##steepdeg", &r.SteepDegrees, 5.0f, 80.0f, "steeper than %.0f deg");
        pick("##low", r.LowLayer); ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##lowh", &r.LowHeight, 0.05f, -1000.0f, 1000.0f, "below %.1f m");
        pick("##high", r.HighLayer); ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::DragFloat("##highh", &r.HighHeight, 0.05f, -1000.0f, 5000.0f, "above %.1f m");
        if (ImGui::Button("Paint all by shape")) Edit(t).AutoPaint(t.Size, r);
        ImGui::SetItemTooltip("Replaces the paint everywhere. Steep wins over low and high.");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Import heightmap")) {
        ImGui::InputText("Image", m_ImportPath, sizeof(m_ImportPath));
        ImGui::SetItemTooltip("A grey image under Assets (8 or 16 bit PNG is best): black = low, white = low + range.\n"
                              "Drop one from the Assets panel. Its top edge is the terrain's -Z side.");
        if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Texture }); !drop.empty())
            std::snprintf(m_ImportPath, sizeof(m_ImportPath), "%s", drop.c_str());
        ImGui::DragFloat("Lowest", &m_ImportLow, 0.1f, -1000.0f, 1000.0f, "%.1f m");
        ImGui::DragFloat("Range", &m_ImportRange, 0.1f, 0.0f, 5000.0f, "%.1f m");
        if (ImGui::Button("Import")) {
            auto copy = std::make_shared<TerrainData>(*t.Data);
            std::string error;
            if (copy->ImportHeightmap(assetDir + m_ImportPath, m_ImportLow, m_ImportRange, &error)) { t.Data = copy; m_Status = "Imported " + std::string(m_ImportPath); }
            else m_Status = "Can't import " + std::string(m_ImportPath) + ": " + error;
        }
        if (!m_Status.empty()) ImGui::TextWrapped("%s", m_Status.c_str());
        ImGui::TreePop();
    }

    // ---- collision and file
    ImGui::SeparatorText("Collision");
    auto* rb = reg.try_get<RigidBodyComponent>(entity);
    if (!rb) {
        ImGui::TextDisabled("Not solid: bodies fall through, the navigation bake ignores it");
        if (ImGui::Button("Make solid")) {
            auto& body = reg.emplace<RigidBodyComponent>(entity);
            body.Type = BodyType::Static;
            body.Shape = ColliderShape::Terrain;
            body.Friction = 0.8f;
            body.Restitution = 0.0f;
        }
        ImGui::SetItemTooltip("Add a static Rigid Body shaped like the terrain (a height field)");
    }
    else if (rb->Shape != ColliderShape::Terrain) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Its Rigid Body is not shaped like the terrain");
        if (ImGui::Button("Use the Terrain shape")) rb->Shape = ColliderShape::Terrain;
    }
    else ImGui::TextDisabled("Solid: a %s height field", rb->Type == BodyType::Static ? "static" : "kinematic");

    ImGui::SeparatorText("Data");
    ImGui::TextDisabled("%s", t.DataPath.empty() ? "(a new file when the scene is saved)" : t.DataPath.c_str());
    ImGui::SetItemTooltip("The heights and paint, under Assets. Written each time the scene is saved.");

    ImGui::PopID();
}
