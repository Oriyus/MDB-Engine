#include "TerrainEditor.h"
#include "AssetBrowser.h"

#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Renderer/Camera.h"
#include "Renderer/Texture.h"
#include "Runtime/ResourceLibrary.h"

#include "Scene/Entity.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cctype>
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

    // An image under Assets for a terrain layer map: a combo of the images in Assets/Textures, plus drops from the
    // Assets panel. Returns true when it changed.
    bool MapPicker(const char* id, const char* what, std::string& path, const std::string& assetDir) {
        bool changed = false;
        ImGui::PushID(id);
        const float clearW = path.empty() ? 0.0f : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearW);
        const std::string preview = path.empty() ? std::string("(no ") + what + " map)" : path;
        if (ImGui::BeginCombo("##map", preview.c_str())) {
            if (ImGui::Selectable("(none)", path.empty())) { path.clear(); changed = true; }
            std::error_code ec;
            for (const auto& file : std::filesystem::recursive_directory_iterator(std::filesystem::u8path(assetDir) / "Textures", ec)) {
                if (!file.is_regular_file()) continue;
                const std::string rel = std::filesystem::relative(file.path(), std::filesystem::u8path(assetDir), ec).generic_string();
                if (AssetBrowser::KindOf(rel) != AssetBrowser::Kind::Texture) continue;
                if (ImGui::Selectable(rel.c_str(), path == rel)) { path = rel; changed = true; }
            }
            ImGui::EndCombo();
        }
        if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Texture }); !drop.empty()) { path = drop; changed = true; }
        ImGui::SetItemTooltip("%s map: %s", what, path.empty() ? "none (choose an image or drop one from Assets)" : path.c_str());
        if (!path.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) { path.clear(); changed = true; }
            ImGui::SetItemTooltip("Remove the %s map", what);
        }
        ImGui::PopID();
        return changed;
    }

    // Look next to a layer's colour texture for its other maps, by the usual name endings ("rock_diff.jpg" ->
    // "rock_nor_gl.png", "rock_rough.jpg", "rock_disp.png"). Fills only what it finds; returns how many it found.
    int FindLayerMaps(TerrainLayer& L, const std::string& colourPath, const std::string& assetDir) {
        namespace fs = std::filesystem;
        auto lower = [](std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
        const fs::path colour = fs::u8path(colourPath);
        std::string base = lower(colour.stem().string());
        for (const char* suffix : { "_basecolor", "_base_color", "_albedo", "_diffuse", "_diff", "_color", "_colour", "_col" }) {
            const std::string s = suffix;
            if (base.size() > s.size() && base.compare(base.size() - s.size(), s.size(), s) == 0) { base.resize(base.size() - s.size()); break; }
        }
        struct Rule { const char* Ends[6]; };
        const Rule normal{ { "_nor_gl", "_normal_gl", "_normal", "_nrm", "_nor", "_n" } };
        const Rule rough { { "_roughness", "_rough", "_rgh", "_r", nullptr, nullptr } };
        const Rule height{ { "_height", "_displacement", "_disp", "_bump", "_h", nullptr } };
        std::string found[3];
        bool directX = false;
        std::error_code ec;
        for (const auto& file : fs::directory_iterator(fs::u8path(assetDir) / colour.parent_path(), ec)) {
            if (!file.is_regular_file()) continue;
            const std::string rel = fs::relative(file.path(), fs::u8path(assetDir), ec).generic_string();
            if (AssetBrowser::KindOf(rel) != AssetBrowser::Kind::Texture) continue;
            const std::string stem = lower(file.path().stem().string());
            if (stem.rfind(base, 0) != 0) continue;
            const std::string rest = stem.substr(base.size());
            const Rule* rules[3] = { &normal, &rough, &height };
            for (int k = 0; k < 3; k++)
                for (const char* e : rules[k]->Ends)
                    if (e && rest == e && found[k].empty()) found[k] = rel;
            if (found[0].empty() && (rest == "_nor_dx" || rest == "_normal_dx")) { found[0] = rel; directX = true; }
        }
        int n = 0;
        if (!found[0].empty()) { L.NormalMap = found[0]; L.NormalFlipY = directX; n++; }
        if (!found[1].empty()) { if (L.RoughnessMap.empty()) L.Roughness = 1.0f; L.RoughnessMap = found[1]; n++; }
        if (!found[2].empty()) { L.HeightMap = found[2]; n++; }
        return n;
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
        else if (m_Tool == Tool::Scatter) {
            BeginScatter(scene, e);                      // instances are entities: the undo history sees them
            m_Stroking = true;
            m_StrokeEntity = e;
        }
        else {
            Edit(tc);                                    // the undo history keeps the data as it was
            m_Stroking = true;
            m_StrokeEntity = e;
            if (m_Tool == Tool::Flatten && m_FlattenAtStart) m_Brush.Target = local.y;
        }
    }
    if (m_Stroking && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || m_StrokeEntity != e || navigating)) m_Stroking = false;

    if (m_Stroking && hit && m_Tool == Tool::Scatter)
        ScatterStroke(scene, e, glm::vec2(local.x, local.z), io.KeyShift, std::clamp(dt, 0.0f, 0.1f));
    else if (m_Stroking && hit) {
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
        else if (m_Tool == Tool::Scatter) col = io.KeyShift ? IM_COL32(255, 90, 80, 230) : IM_COL32(120, 230, 90, 230);
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
        "Paint: left drag paints the chosen layer   [ ]: brush size",
        "Scatter: left drag places the items   Shift: erase the group's   [ ]: brush size" };
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
        { Tool::Scatter, "Scatter", "Paint models or prefabs onto the ground: trees, rocks, bushes (Shift: erase)" },
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
        if (m_Tool == Tool::Scatter) DrawScatterInspector(scene, entity, assetDir);
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
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##variation", &L.Variation, 0.0f, 1.0f, "variation %.2f");
        ImGui::SetItemTooltip("Stochastic tiling: the texture is laid as randomly shifted and turned copies, blended\n"
                              "along its own dark lines, so the repeat stops showing across big areas (3 samples).\n"
                              "0: a plain repeat. Lower it for textures with a direction (rows, light from one side).");
        // Normal / roughness / height maps, packed into one texture by the resource library (cached; re-read when a
        // file changes, checked here every frame while the terrain is selected)
        const int mapCount = !L.NormalMap.empty() + !L.RoughnessMap.empty() + !L.HeightMap.empty();
        const std::string mapsLabel = mapCount ? "Maps (" + std::to_string(mapCount) + ")###maps" : std::string("Maps###maps");
        if (ImGui::TreeNodeEx(mapsLabel.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
            MapPicker("normal", "normal", L.NormalMap, assetDir);
            if (!L.NormalMap.empty()) {
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
                ImGui::SliderFloat("##nstrength", &L.NormalStrength, 0.0f, 2.0f, "normal strength %.2f");
                ImGui::SameLine();
                ImGui::Checkbox("DirectX", &L.NormalFlipY);
                ImGui::SetItemTooltip("The map's green channel points down the image (DirectX / Unreal style, files\n"
                                      "often named _nor_dx). Off: OpenGL style (_nor_gl, Blender, Godot, Unity).");
            }
            const bool hadRough = !L.RoughnessMap.empty();
            if (MapPicker("rough", "roughness", L.RoughnessMap, assetDir) && !hadRough && !L.RoughnessMap.empty())
                L.Roughness = 1.0f;                       // the slider multiplies the map: start from the map as it is
            MapPicker("height", "height", L.HeightMap, assetDir);
            ImGui::BeginDisabled(!L.Texture);
            if (ImGui::Button("Find maps next to the texture")) {
                std::string colour = L.Texture ? registry.TextureName(L.Texture) : "";
                if (colour.rfind("texture:", 0) == 0) colour = colour.substr(8);
                if (!colour.empty()) FindLayerMaps(L, colour, assetDir);
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Fill the maps from images beside the colour texture with the same name and the\n"
                                  "usual endings: _normal / _nor_gl / _nor_dx / _nrm, _roughness / _rough,\n"
                                  "_height / _disp / _displacement.");
            ImGui::TextDisabled("Roughness map x the roughness slider. Height drives the height blend\n"
                                "and the stochastic seams (else the colour's brightness).");
            ImGui::TreePop();
        }
        L.Surface = resources.LoadTerrainSurface(L);
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

// ---------------------------------------------------------------- scatter

namespace {
    int64_t CellKey(int x, int z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); }
}

entt::entity TerrainEditor::FindGroup(Scene& scene, entt::entity terrain, bool create) {
    auto& reg = scene.Registry();
    // A top-level entity: under the terrain, selecting the terrain would outline every instance
    (void)terrain;
    for (auto [e, tag] : reg.view<TagComponent>().each())
        if (tag.Tag == m_Scatter.Group && !reg.all_of<ParentComponent>(e)) return e;
    if (!create) return entt::null;
    return scene.CreateEntity(m_Scatter.Group.empty() ? "Scatter" : m_Scatter.Group).GetHandle();
}

void TerrainEditor::BeginScatter(Scene& scene, entt::entity terrain) {
    m_ScatterGrid.clear();
    m_ScatterCell = std::max(0.25f, m_Scatter.Spacing);
    m_ScatterBudget = 0.0f;
    m_ScatterGroup = FindGroup(scene, terrain, false);
    if (m_ScatterGroup == entt::null) return;
    const glm::mat4 toLocal = glm::inverse(scene.GetWorldMatrix(terrain));
    for (entt::entity c : scene.GetChildren(m_ScatterGroup)) {
        const glm::vec3 p = glm::vec3(toLocal * scene.GetWorldMatrix(c)[3]);
        AddPoint(glm::vec2(p.x, p.z));
    }
}

void TerrainEditor::AddPoint(const glm::vec2& p) {
    const int x = (int)std::floor(p.x / m_ScatterCell), z = (int)std::floor(p.y / m_ScatterCell);
    m_ScatterGrid[CellKey(x, z)].push_back(p);
}

bool TerrainEditor::SpacingFree(const glm::vec2& p) const {
    const float s = m_Scatter.Spacing;
    if (s <= 0.0f) return true;
    const int x = (int)std::floor(p.x / m_ScatterCell), z = (int)std::floor(p.y / m_ScatterCell);
    const int reach = (int)std::ceil(s / m_ScatterCell);
    for (int dz = -reach; dz <= reach; dz++)
        for (int dx = -reach; dx <= reach; dx++) {
            const auto it = m_ScatterGrid.find(CellKey(x + dx, z + dz));
            if (it == m_ScatterGrid.end()) continue;
            for (const glm::vec2& q : it->second) if (glm::dot(q - p, q - p) < s * s) return false;
        }
    return true;
}

int TerrainEditor::CountIn(const glm::vec2& c, float r) const {
    const int x0 = (int)std::floor((c.x - r) / m_ScatterCell), x1 = (int)std::floor((c.x + r) / m_ScatterCell);
    const int z0 = (int)std::floor((c.y - r) / m_ScatterCell), z1 = (int)std::floor((c.y + r) / m_ScatterCell);
    int n = 0;
    if ((int64_t)(x1 - x0 + 1) * (z1 - z0 + 1) > (int64_t)m_ScatterGrid.size() * 4) {   // a huge brush: walk the points
        for (const auto& kv : m_ScatterGrid) for (const glm::vec2& q : kv.second) n += glm::dot(q - c, q - c) <= r * r;
        return n;
    }
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++) {
            const auto it = m_ScatterGrid.find(CellKey(x, z));
            if (it == m_ScatterGrid.end()) continue;
            for (const glm::vec2& q : it->second) n += glm::dot(q - c, q - c) <= r * r;
        }
    return n;
}

bool TerrainEditor::TryPlace(Scene& scene, entt::entity terrain, const glm::vec2& p) {
    auto& reg = scene.Registry();
    const auto& tc = reg.get<TerrainComponent>(terrain);
    const ScatterSettings& s = m_Scatter;
    if (!tc.Data || !m_CreateAsset) return false;
    // ---- the rules
    float h;
    if (!tc.Data->HeightAt(tc.Size, p.x, p.y, h)) return false;
    const glm::vec3 n = tc.Data->NormalAt(tc.Size, p.x, p.y);
    if (glm::degrees(std::acos(std::clamp(n.y, -1.0f, 1.0f))) > s.MaxSlope) return false;
    if (s.UseHeight && (h < s.MinHeight || h > s.MaxHeight)) return false;
    if (s.Layer >= 0 && s.Layer < 4) {
        const glm::vec4 w = tc.Data->WeightsAt(p.x / tc.Size.x + 0.5f, p.y / tc.Size.y + 0.5f);
        if (w[s.Layer] < s.LayerMin) return false;
    }
    if (!SpacingFree(p)) return false;
    // ---- which item
    float total = 0.0f;
    for (const auto& it : s.Items) if (!it.Path.empty()) total += std::max(0.0f, it.Weight);
    if (total <= 0.0f) return false;
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    float pick = U(m_Rng) * total;
    const ScatterItem* item = nullptr;
    for (const auto& it : s.Items) {
        if (it.Path.empty() || it.Weight <= 0.0f) continue;
        item = &it;
        if ((pick -= it.Weight) <= 0.0f) break;
    }
    if (!item) return false;
    const float yaw = s.RandomYaw ? U(m_Rng) * 6.2831853f : 0.0f;
    const float scale = item->Scale * glm::mix(std::min(s.ScaleMin, s.ScaleMax), std::max(s.ScaleMin, s.ScaleMax), U(m_Rng));

    const entt::entity e = m_CreateAsset(item->Path);
    if (e == entt::null || !reg.valid(e)) return false;
    if (m_ScatterGroup == entt::null || !reg.valid(m_ScatterGroup)) m_ScatterGroup = FindGroup(scene, terrain, true);

    // ---- what it is: static, wind, a body
    std::vector<entt::entity> stack{ e };
    while (!stack.empty()) {
        const entt::entity x = stack.back(); stack.pop_back();
        if (auto* mr = reg.try_get<MeshRendererComponent>(x)) {
            if (s.Static) mr->Static = true;
            if (s.Wind > 0.0f) mr->Wind = s.Wind;
        }
        for (entt::entity c : scene.GetChildren(x)) stack.push_back(c);
    }
    if (s.Collider != ScatterCollider::Keep) {
        reg.remove<RigidBodyComponent>(e);
        if (s.Collider == ScatterCollider::Trunk) {
            auto& rb = reg.emplace<RigidBodyComponent>(e);
            rb.Type = BodyType::Static;
            rb.Shape = ColliderShape::Box;
            rb.Size = glm::vec3(s.TrunkWidth, s.TrunkHeight, s.TrunkWidth);
            rb.Center = glm::vec3(0.0f, s.TrunkHeight * 0.5f, 0.0f);
            rb.Friction = 0.6f;
        }
        else if (s.Collider == ScatterCollider::Mesh && reg.all_of<MeshRendererComponent>(e)) {
            auto& rb = reg.emplace<RigidBodyComponent>(e);
            rb.Type = BodyType::Static;
            rb.Shape = ColliderShape::Mesh;
        }
    }

    // ---- where: on the ground, turned about the up axis, leaning with the slope by Align
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::quat none(1.0f, 0.0f, 0.0f, 0.0f);
    glm::quat tilt = none;
    if (s.Align > 0.0f && n.y < 0.9999f) tilt = glm::slerp(none, glm::rotation(up, glm::normalize(n)), std::clamp(s.Align, 0.0f, 1.0f));
    const glm::quat rot = tilt * glm::angleAxis(yaw, up);
    const glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(p.x, h - s.Sink, p.y)) * glm::mat4_cast(rot) *
                            glm::scale(glm::mat4(1.0f), glm::vec3(scale));
    scene.SetParent(e, m_ScatterGroup, false);
    scene.SetWorldMatrix(e, scene.GetWorldMatrix(terrain) * local);
    AddPoint(p);
    return true;
}

void TerrainEditor::ScatterStroke(Scene& scene, entt::entity terrain, const glm::vec2& centre, bool erase, float dt) {
    const float r = m_Brush.Radius;
    const float inner = r * (1.0f - std::clamp(m_Brush.Falloff, 0.0f, 1.0f));
    auto weightAt = [&](float d) {                       // the brush's strength at a distance from its centre
        if (d <= inner) return 1.0f;
        const float t = std::clamp((d - inner) / std::max(1e-4f, r - inner), 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    };
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    if (erase) {
        m_ScatterGroup = FindGroup(scene, terrain, false);
        if (m_ScatterGroup == entt::null) return;
        const glm::mat4 toLocal = glm::inverse(scene.GetWorldMatrix(terrain));
        const float chance = std::clamp(m_Brush.Strength * dt * 8.0f, 0.0f, 1.0f);
        bool removed = false;
        for (entt::entity c : scene.GetChildren(m_ScatterGroup)) {
            const glm::vec3 p = glm::vec3(toLocal * scene.GetWorldMatrix(c)[3]);
            const float d = glm::length(glm::vec2(p.x, p.z) - centre);
            if (d > r || U(m_Rng) > chance * weightAt(d)) continue;
            scene.DestroyEntity(Entity(c, &scene));
            removed = true;
        }
        if (removed) BeginScatter(scene, terrain);       // the points again, without the removed ones
        return;
    }
    // Fill the brush up to Density: Strength 1 fills an empty brush in about half a second (at least 8 a second)
    const float target = m_Scatter.Density * 3.14159265f * r * r / 100.0f;
    const int have = CountIn(centre, r);
    if (have >= target) { m_ScatterBudget = 0.0f; return; }
    m_ScatterBudget += std::max(target, 4.0f) * m_Brush.Strength * 2.0f * dt;
    int tries = std::min((int)m_ScatterBudget, 64);
    m_ScatterBudget -= (float)tries;
    int placed = 0;
    while (tries-- > 0 && have + placed < target) {
        // a point in the disc, kept by the falloff; up to 8 attempts per try (spacing and the rules reject some)
        for (int a = 0; a < 8; a++) {
            const float ang = U(m_Rng) * 6.2831853f, d = r * std::sqrt(U(m_Rng));
            if (U(m_Rng) > weightAt(d)) continue;
            if (TryPlace(scene, terrain, centre + glm::vec2(std::cos(ang), std::sin(ang)) * d)) { placed++; break; }
        }
    }
}

int TerrainEditor::ScatterFill(Scene& scene, entt::entity terrain) {
    const auto& tc = scene.Registry().get<TerrainComponent>(terrain);
    const glm::vec2 size = tc.Size;
    BeginScatter(scene, terrain);
    m_Rng.seed(m_Scatter.Seed);
    const int target = std::min(20000, (int)(m_Scatter.Density * size.x * size.y / 100.0f));
    int have = 0;
    for (const auto& kv : m_ScatterGrid) have += (int)kv.second.size();
    std::uniform_real_distribution<float> U(-0.5f, 0.5f);
    int placed = 0;
    for (int tries = (target - have) * 6; tries > 0 && have + placed < target; tries--)
        if (TryPlace(scene, terrain, glm::vec2(U(m_Rng) * size.x, U(m_Rng) * size.y))) placed++;
    return placed;
}

int TerrainEditor::ScatterResnap(Scene& scene, entt::entity terrain) {
    const auto& tc = scene.Registry().get<TerrainComponent>(terrain);
    const entt::entity g = FindGroup(scene, terrain, false);
    if (g == entt::null || !tc.Data) return 0;
    const glm::mat4 T = scene.GetWorldMatrix(terrain), toLocal = glm::inverse(T);
    int n = 0;
    for (entt::entity c : scene.GetChildren(g)) {
        glm::mat4 local = toLocal * scene.GetWorldMatrix(c);
        float h;
        if (!tc.Data->HeightAt(tc.Size, local[3].x, local[3].z, h)) continue;
        local[3].y = h - m_Scatter.Sink;
        scene.SetWorldMatrix(c, T * local);
        n++;
    }
    return n;
}

void TerrainEditor::DrawScatterInspector(Scene& scene, entt::entity terrain, const std::string& assetDir) {
    ScatterSettings& s = m_Scatter;
    ImGui::PushID("scatter");
    // ---- items: models and prefabs, dropped from Assets or picked from the list
    auto listAssets = [&](const std::string& current) -> std::string {
        std::string chosen;
        std::error_code ec;
        const auto root = std::filesystem::u8path(assetDir);
        for (const char* folder : { "Models", "Prefabs" })
            for (const auto& file : std::filesystem::recursive_directory_iterator(root / folder, ec)) {
                if (!file.is_regular_file()) continue;
                const std::string rel = std::filesystem::relative(file.path(), root, ec).generic_string();
                const auto kind = AssetBrowser::KindOf(rel);
                if (kind != AssetBrowser::Kind::Model && kind != AssetBrowser::Kind::Prefab) continue;
                if (ImGui::Selectable(rel.c_str(), rel == current)) chosen = rel;
            }
        return chosen;
    };
    ImGui::TextUnformatted("Items");
    int remove = -1;
    for (int i = 0; i < (int)s.Items.size(); i++) {
        ScatterItem& it = s.Items[i];
        ImGui::PushID(i);
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(w * 0.5f);
        if (ImGui::BeginCombo("##item", it.Path.empty() ? "(pick a model or prefab)" : it.Path.c_str())) {
            if (auto c = listAssets(it.Path); !c.empty()) it.Path = c;
            ImGui::EndCombo();
        }
        if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Model, AssetBrowser::Kind::Prefab }); !drop.empty()) it.Path = drop;
        ImGui::SameLine(); ImGui::SetNextItemWidth(w * 0.2f);
        ImGui::DragFloat("##weight", &it.Weight, 0.05f, 0.0f, 100.0f, "x%.2f");
        ImGui::SetItemTooltip("How often it is picked against the others");
        ImGui::SameLine(); ImGui::SetNextItemWidth(w * 0.18f);
        ImGui::DragFloat("##scale", &it.Scale, 0.01f, 0.01f, 100.0f, "s %.2f");
        ImGui::SetItemTooltip("Its own scale (times the random scale).\nModels arrive 2 m tall, kit pieces at their own size.");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = i;
        ImGui::PopID();
    }
    if (remove >= 0) s.Items.erase(s.Items.begin() + remove);
    if (ImGui::Button("+ Item")) s.Items.push_back({});
    ImGui::SetItemTooltip("Or drop models and prefabs from the Assets panel on this button");
    if (auto drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Model, AssetBrowser::Kind::Prefab }); !drop.empty())
        s.Items.push_back({ drop, 1.0f, 1.0f });
    if (s.Items.empty()) { ImGui::SameLine(); ImGui::TextDisabled("drop a model or prefab here"); }

    char group[128];
    std::snprintf(group, sizeof(group), "%s", s.Group.c_str());
    if (ImGui::InputText("Group", group, sizeof(group)) && group[0]) s.Group = group;
    ImGui::SetItemTooltip("The instances go into a child of the terrain with this name.\nErasing and re-snapping touch only this group.");
    const entt::entity g = FindGroup(scene, terrain, false);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", g == entt::null ? size_t(0) : scene.GetChildren(g).size());

    ImGui::DragFloat("Density", &s.Density, 0.05f, 0.01f, 400.0f, "%.2f per 100 m2");
    ImGui::DragFloat("Spacing", &s.Spacing, 0.05f, 0.0f, 100.0f, "%.2f m");
    ImGui::SetItemTooltip("The least distance between two instances of the group");
    ImGui::DragFloatRange2("Scale", &s.ScaleMin, &s.ScaleMax, 0.01f, 0.01f, 100.0f, "%.2f", "%.2f");
    ImGui::Checkbox("Random turn", &s.RandomYaw);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##align", &s.Align, 0.0f, 1.0f, "lean with slope %.2f");
    ImGui::SetItemTooltip("0: upright (trees). 1: tilted to the ground (rocks).");
    ImGui::DragFloat("Sink", &s.Sink, 0.01f, -10.0f, 10.0f, "%.2f m");
    ImGui::SetItemTooltip("How far into the ground, so the base shows no gap on slopes");

    if (ImGui::TreeNode("Rules")) {
        ImGui::SliderFloat("Max slope", &s.MaxSlope, 0.0f, 90.0f, "%.0f deg");
        ImGui::Checkbox("Height range", &s.UseHeight);
        ImGui::BeginDisabled(!s.UseHeight);
        ImGui::DragFloatRange2("##height", &s.MinHeight, &s.MaxHeight, 0.05f, -1000.0f, 5000.0f, "%.1f m", "%.1f m");
        ImGui::EndDisabled();
        const char* layers[] = { "Anywhere", "On layer 1", "On layer 2", "On layer 3", "On layer 4" };
        int v = s.Layer + 1;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        if (ImGui::Combo("##layer", &v, layers, 5)) s.Layer = v - 1;
        ImGui::SameLine();
        ImGui::BeginDisabled(s.Layer < 0);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##layermin", &s.LayerMin, 0.05f, 1.0f, "painted at least %.2f");
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Instances")) {
        ImGui::Checkbox("Static", &s.Static);
        ImGui::SetItemTooltip("Still level geometry: merged into batches in Play, and the grass avoids it");
        ImGui::SliderFloat("Wind", &s.Wind, 0.0f, 1.0f, s.Wind > 0.0f ? "%.2f" : "as made");
        ImGui::SetItemTooltip("Sway in the scene wind (1 = a tree). 0 keeps the model's or prefab's own setting.");
        const char* bodies[] = { "As made", "None", "Trunk box", "Mesh" };
        int c = (int)s.Collider;
        if (ImGui::Combo("Collider", &c, bodies, 4)) s.Collider = (ScatterCollider)c;
        ImGui::SetItemTooltip("As made: a prefab keeps its own body.\nTrunk box: a static box standing at the base.\nMesh: the whole shape (costly for trees).");
        if (s.Collider == ScatterCollider::Trunk) {
            ImGui::DragFloat("Trunk width", &s.TrunkWidth, 0.01f, 0.05f, 20.0f, "%.2f m");
            ImGui::DragFloat("Trunk height", &s.TrunkHeight, 0.05f, 0.1f, 100.0f, "%.2f m");
        }
        ImGui::TreePop();
    }
    if (ImGui::Button("Fill terrain")) m_Status = "Scattered " + std::to_string(ScatterFill(scene, terrain));
    ImGui::SetItemTooltip("Scatter over the whole terrain by the rules, up to the density");
    ImGui::SameLine();
    int seed = (int)s.Seed;
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::DragInt("Seed##scatter", &seed, 1.0f, 0, 1 << 30)) s.Seed = (uint32_t)std::max(seed, 0);
    if (ImGui::Button("Re-snap to ground")) m_Status = "Re-snapped " + std::to_string(ScatterResnap(scene, terrain));
    ImGui::SetItemTooltip("Stand the group's instances on the ground again after sculpting (keeps their turn and lean)");
    ImGui::SameLine();
    if (ImGui::Button("Clear group") && g != entt::null) { scene.DestroyEntity(Entity(g, &scene)); m_Status = "Cleared " + s.Group; }
    if (!m_Status.empty()) ImGui::TextDisabled("%s", m_Status.c_str());
    ImGui::PopID();
}
