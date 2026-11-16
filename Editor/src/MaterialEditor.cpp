#include "MaterialEditor.h"
#include "AssetBrowser.h"

#include "Renderer/MaterialAsset.h"
#include "Runtime/ResourceLibrary.h"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>

using namespace Engine;
using json = nlohmann::json;
namespace fs = std::filesystem;

MaterialEditor::MaterialEditor(std::string engineDir, std::string assetDir)
    : m_EngineDir(engineDir), m_AssetDir(assetDir), m_Preview(std::move(engineDir), std::move(assetDir)) {
    m_Preview.Closable = false;
}

bool MaterialEditor::IsDirty() const { return !m_Path.empty() && m_File != m_Saved; }

bool MaterialEditor::Holds(const MaterialAsset& m) const { return IsDirty() && m.Path == m_Path; }

void MaterialEditor::Open(const std::string& path) {
    if (path == m_Path) return;
    if (IsDirty()) { m_PendingOpen = path; m_AskSwitch = true; return; }
    DoOpen(path);
}

void MaterialEditor::DoOpen(const std::string& path) {
    m_Path = path;
    m_Asset = m_Resources && !path.empty() ? m_Resources->LoadMaterial(path) : nullptr;
    if (m_Asset && m_Resources) m_Resources->ReloadMaterial(path);       // what is on disk now
    m_File = m_Asset ? m_Asset->File : json::object();
    m_Saved = m_File;
    m_Undo = { m_File };
    m_UndoPos = 0;
    m_Status.clear();
    m_Preview.SetAsset(path);
}

void MaterialEditor::Apply() {
    if (m_Resources && !m_Path.empty()) m_Resources->SetMaterialFile(m_Path, m_File);
}

void MaterialEditor::Commit() {
    if (m_UndoPos >= 0 && m_UndoPos < (int)m_Undo.size() && m_Undo[m_UndoPos] == m_File) return;
    m_Undo.resize(m_UndoPos + 1);
    m_Undo.push_back(m_File);
    if (m_Undo.size() > 100) m_Undo.erase(m_Undo.begin());
    m_UndoPos = (int)m_Undo.size() - 1;
}

void MaterialEditor::Undo() {
    if (m_UndoPos <= 0) return;
    m_File = m_Undo[--m_UndoPos];
    Apply();
}

void MaterialEditor::Redo() {
    if (m_UndoPos + 1 >= (int)m_Undo.size()) return;
    m_File = m_Undo[++m_UndoPos];
    Apply();
}

bool MaterialEditor::Save() {
    if (!m_Resources || m_Path.empty()) return false;
    std::string error;
    if (!m_Resources->SaveMaterial(m_Path, m_File, &error)) { m_Status = "Save failed: " + error; return false; }
    m_File = m_Asset->File;                       // as written (with the version)
    m_Saved = m_File;
    if (m_UndoPos >= 0 && m_UndoPos < (int)m_Undo.size()) m_Undo[m_UndoPos] = m_File;
    m_Status = "Saved " + m_Path;
    return true;
}

void MaterialEditor::Revert() {
    if (!m_Resources || m_Path.empty()) return;
    m_Resources->ReloadMaterial(m_Path);
    m_File = m_Asset ? m_Asset->File : json::object();
    m_Saved = m_File;
    Commit();
    m_Status = "Read " + m_Path + " again";
}

void MaterialEditor::Render(float dt) {
    if (m_Resources) m_Preview.Render(dt, *m_Resources);
}

void MaterialEditor::ScanAssets() {
    if (ImGui::GetTime() - m_LastScan < 2.0) return;
    m_LastScan = ImGui::GetTime();
    m_Images.clear();
    m_Materials.clear();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(fs::u8path(m_AssetDir), fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string rel = fs::relative(it->path(), fs::u8path(m_AssetDir), ec).generic_string();
        const auto kind = AssetBrowser::KindOf(rel);
        if (kind == AssetBrowser::Kind::Texture) m_Images.push_back(rel);
        else if (kind == AssetBrowser::Kind::Material) m_Materials.push_back(rel);
    }
    std::sort(m_Images.begin(), m_Images.end());
    std::sort(m_Materials.begin(), m_Materials.end());
}

// ---------------------------------------------------------------- widgets
bool MaterialEditor::Value(const char* key, const std::function<bool(MaterialDesc&)>& edit, const char* key2) {
    ImGui::PushID(key);
    const bool variant = m_File.contains("parent") && m_File["parent"].is_string() && !m_File["parent"].get<std::string>().empty();
    if (variant) {
        // The dot: this variant changes the value (click: back to the parent's)
        const bool own = m_File.contains(key) || (key2 && m_File.contains(key2));
        const float w = ImGui::GetFrameHeight() * 0.6f;
        if (own) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.7f, 0.25f, 0.85f));
            if (ImGui::Button("##own", ImVec2(w, ImGui::GetFrameHeight()))) {
                m_File.erase(key);
                if (key2) m_File.erase(key2);
                Apply();
            }
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("This variant changes the value. Click to use the parent's again.");
        }
        else {
            ImGui::Dummy(ImVec2(w, ImGui::GetFrameHeight()));
            ImGui::SetItemTooltip("The parent's value");
        }
        ImGui::SameLine();
    }
    MaterialDesc d = m_Asset->Desc;
    const bool changed = edit(d);
    if (changed) {
        const json j = MaterialDescToJson(d);
        if (j.contains(key)) m_File[key] = j[key];
        if (key2 && j.contains(key2)) m_File[key2] = j[key2];
        Apply();
    }
    ImGui::PopID();
    return changed;
}

bool MaterialEditor::MapPicker(const char* label, std::string& path, const char* tip) {
    bool changed = false;
    ImGui::PushID(label);
    const float thumb = ImGui::GetFrameHeight();
    const uint32_t tex = !path.empty() && TextureId ? TextureId(path) : 0;
    if (tex) ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(thumb, thumb), ImVec2(0, 1), ImVec2(1, 0));
    else ImGui::Dummy(ImVec2(thumb, thumb));
    if (tex && ImGui::BeginItemTooltip()) {
        ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(160, 160), ImVec2(0, 1), ImVec2(1, 0));
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    const float clearW = path.empty() ? 0.0f : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::CalcItemWidth() - thumb - ImGui::GetStyle().ItemSpacing.x - clearW));
    const std::string shown = path.empty() ? "(none)" : path;
    if (ImGui::BeginCombo("##map", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        ScanAssets();
        if (ImGui::Selectable("(none)", path.empty())) { path.clear(); changed = true; }
        for (const auto& p : m_Images)
            if (ImGui::Selectable(p.c_str(), p == path)) { path = p; changed = true; }
        ImGui::EndCombo();
    }
    if (const std::string drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Texture }); !drop.empty()) { path = drop; changed = true; }
    if (!path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) { path.clear(); changed = true; }
        ImGui::SetItemTooltip("No map");
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

bool MaterialEditor::MaterialPicker(const char* label, std::string& path) {
    bool changed = false;
    const std::string shown = path.empty() ? "(none)" : path;
    if (ImGui::BeginCombo(label, shown.c_str(), ImGuiComboFlags_HeightLarge)) {
        ScanAssets();
        if (ImGui::Selectable("(none)", path.empty())) { path.clear(); changed = true; }
        for (const auto& p : m_Materials)
            if (p != m_Path && ImGui::Selectable(p.c_str(), p == path)) { path = p; changed = true; }
        ImGui::EndCombo();
    }
    if (const std::string drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Material }); !drop.empty() && drop != m_Path) { path = drop; changed = true; }
    return changed;
}

// ---------------------------------------------------------------- the window
void MaterialEditor::Draw(bool* open) {
    m_Focused = false;
    ImGui::SetNextWindowSize(ImVec2(420, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Material Editor", open, ImGuiWindowFlags_MenuBar)) { ImGui::End(); return; }
    m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // The file changed on disk (or another window saved it) and nothing here is unsaved: follow it
    if (m_Asset && !IsDirty() && m_Asset->File != m_File) { m_File = m_Asset->File; m_Saved = m_File; Commit(); }

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save", "Ctrl+S", false, IsDirty())) Save();
            if (ImGui::MenuItem("Revert", nullptr, false, IsDirty())) Revert();
            ImGui::Separator();
            if (ImGui::BeginMenu("Open")) {
                ScanAssets();
                if (m_Materials.empty()) ImGui::TextDisabled("No .mat files under Assets");
                for (const auto& p : m_Materials) if (ImGui::MenuItem(p.c_str(), nullptr, p == m_Path)) Open(p);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_UndoPos > 0)) Undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_UndoPos + 1 < (int)m_Undo.size())) Redo();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (!m_Asset) {
        ImGui::TextWrapped("Double-click a material (.mat) in Assets to edit it, or right-click a folder there > New Material.");
        ImGui::End();
        return;
    }

    if (m_Focused && !ImGui::GetIO().WantTextInput) {
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (ImGui::GetIO().KeyShift) Redo(); else Undo(); }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
    }

    // Name, state, parent
    ImGui::TextUnformatted(m_Path.c_str());
    if (IsDirty()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "(unsaved)"); }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 2.0f * (70.0f + ImGui::GetStyle().ItemSpacing.x) + ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(!IsDirty());
    if (ImGui::Button("Save", ImVec2(70, 0))) Save();
    ImGui::SameLine();
    if (ImGui::Button("Revert", ImVec2(70, 0))) Revert();
    ImGui::EndDisabled();
    if (m_Asset->Missing) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "No such file: saving creates it");
    if (!m_Asset->Error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", m_Asset->Error.c_str());
    if (!m_Status.empty()) ImGui::TextDisabled("%s", m_Status.c_str());

    m_Preview.SetAsset(m_Path);
    m_Preview.Draw(std::clamp(ImGui::GetContentRegionAvail().x * 0.75f, 160.0f, 300.0f) + ImGui::GetFrameHeightWithSpacing() * 2.0f);

    ImGui::BeginChild("##values", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::PushItemWidth(-ImGui::GetContentRegionAvail().x * 0.36f);
    {
        std::string parent = m_File.value("parent", std::string());
        if (MaterialPicker("Parent", parent)) {
            if (parent.empty()) {
                // Keep how it looks: its own values become every value
                json own = MaterialDescToJson(m_Asset->Desc);
                own.erase("shader");
                for (auto it = own.begin(); it != own.end(); ++it) m_File[it.key()] = it.value();
                m_File.erase("parent");
            }
            else m_File["parent"] = parent;
            Apply();
        }
        ImGui::SetItemTooltip("A variant starts as its parent and keeps following it, except for the values it changes "
                              "(marked with a dot; click the dot to use the parent's value again)");
        ImGui::BeginDisabled();
        ImGui::LabelText("Shader", "%s", m_Asset->Desc.Shader.c_str());
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Standard: physically based, metal / roughness (lit.frag). Shader graphs come later.");
    }

    ImGui::SeparatorText("Surface");
    Value("color", [](MaterialDesc& d) { return ImGui::ColorEdit3("Color", &d.Color.x); });
    Value("albedoMap", [this](MaterialDesc& d) { return MapPicker("Color map", d.AlbedoMap, "sRGB colour (and alpha); multiplied by Color"); });
    Value("alphaMode", [](MaterialDesc& d) {
        static const char* kModes[] = { "Opaque", "Cutout", "Blend" };
        int m = (int)d.Mode;
        if (!ImGui::Combo("Alpha mode", &m, kModes, 3)) return false;
        d.Mode = (AlphaMode)m;
        return true;
    });
    ImGui::SetItemTooltip("Opaque ignores alpha. Cutout drops pixels below the cutoff (leaves, fences). Blend is see-through (glass).");
    if (m_Asset->Desc.Mode != AlphaMode::Opaque)
        Value("alpha", [](MaterialDesc& d) { return ImGui::SliderFloat("Alpha", &d.Alpha, 0.0f, 1.0f); });
    if (m_Asset->Desc.Mode == AlphaMode::Cutout)
        Value("alphaCutoff", [](MaterialDesc& d) { return ImGui::SliderFloat("Cutoff", &d.AlphaCutoff, 0.0f, 1.0f); });
    Value("doubleSided", [](MaterialDesc& d) { return ImGui::Checkbox("Double-sided", &d.DoubleSided); });
    Value("castShadows", [](MaterialDesc& d) { return ImGui::Checkbox("Cast shadows", &d.CastShadows); });
    Value("unlit", [](MaterialDesc& d) { return ImGui::Checkbox("Unlit", &d.Unlit); });
    ImGui::SetItemTooltip("Its colour only, no lighting or shadows (signs, screens, markers)");

    ImGui::SeparatorText("Metal and roughness");
    Value("metallic", [](MaterialDesc& d) { return ImGui::SliderFloat("Metallic", &d.Metallic, 0.0f, 1.0f); });
    Value("roughness", [](MaterialDesc& d) { return ImGui::SliderFloat("Roughness", &d.Roughness, 0.0f, 1.0f); });
    Value("specular", [](MaterialDesc& d) { return ImGui::SliderFloat("Specular", &d.Specular, 0.0f, 1.0f); });
    ImGui::SetItemTooltip("Reflection of a non-metal: 1 = the usual 4%%, 0 = none (matte bark, cloth, clay)");
    Value("metalRoughMap", [this](MaterialDesc& d) {
        return MapPicker("Metal / rough map", d.MetalRoughMap, "Packed like glTF: green = roughness, blue = metallic (multiplied by the sliders)");
    });
    ImGui::BeginDisabled(!m_Asset->Desc.MetalRoughMap.empty());
    Value("roughnessMap", [this](MaterialDesc& d) { return MapPicker("Roughness map", d.RoughnessMap, "Grey: 0 smooth .. 1 rough (multiplied by Roughness)"); });
    Value("metallicMap", [this](MaterialDesc& d) { return MapPicker("Metallic map", d.MetallicMap, "Grey: 0 not metal .. 1 metal (multiplied by Metallic)"); });
    ImGui::EndDisabled();
    if (!m_Asset->Desc.MetalRoughMap.empty() && (!m_Asset->Desc.RoughnessMap.empty() || !m_Asset->Desc.MetallicMap.empty()))
        ImGui::TextDisabled("The packed map is used on meshes; terrain layers use the grey roughness map.");

    ImGui::SeparatorText("Normal map");
    Value("normalMap", [this](MaterialDesc& d) { return MapPicker("Normal map", d.NormalMap, "Tangent space, OpenGL style (green = up the image) unless DirectX is on"); });
    Value("normalStrength", [](MaterialDesc& d) { return ImGui::SliderFloat("Strength", &d.NormalStrength, 0.0f, 2.0f); });
    Value("normalFlipY", [](MaterialDesc& d) { return ImGui::Checkbox("DirectX (green down)", &d.NormalFlipY); });

    ImGui::SeparatorText("Emission");
    Value("emissive", [](MaterialDesc& d) { return ImGui::ColorEdit3("Emissive", &d.Emissive.x, ImGuiColorEditFlags_Float); });
    Value("emissiveIntensity", [](MaterialDesc& d) { return ImGui::DragFloat("Intensity", &d.EmissiveIntensity, 0.05f, 0.0f, 100.0f); });
    Value("emissiveMap", [this](MaterialDesc& d) {
        if (!MapPicker("Emissive map", d.EmissiveMap, "sRGB; multiplied by Emissive x Intensity")) return false;
        if (!d.EmissiveMap.empty() && d.Emissive == glm::vec3(0.0f)) d.Emissive = glm::vec3(1.0f);   // so the map shows
        return true;
    }, "emissive");

    ImGui::SeparatorText("Height");
    Value("heightMap", [this](MaterialDesc& d) { return MapPicker("Height map", d.HeightMap, "Grey: terrain layers blend by it (stones poke through grass). Not used on meshes yet."); });

    ImGui::SeparatorText("Placement");
    Value("uvScale", [](MaterialDesc& d) { return ImGui::DragFloat2("UV scale", &d.UVScale.x, 0.01f, -1000.0f, 1000.0f); });
    ImGui::SetItemTooltip("Repeats of every map across the mesh's texture coordinates (terrain layers use their own Tiling)");
    Value("uvOffset", [](MaterialDesc& d) { return ImGui::DragFloat2("UV offset", &d.UVOffset.x, 0.01f); });

    ImGui::PopItemWidth();
    ImGui::Spacing();
    ImGui::TextDisabled("Drag the file from Assets onto an object in the Viewport, a Mesh Renderer's material slot or a terrain layer.");
    ImGui::EndChild();

    if (!ImGui::IsAnyItemActive()) Commit();

    // Another material asked for while this one has unsaved changes
    if (m_AskSwitch) { ImGui::OpenPopup("Unsaved material"); m_AskSwitch = false; }
    if (ImGui::BeginPopupModal("Unsaved material", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has unsaved changes.", m_Path.c_str());
        if (ImGui::Button("Save", ImVec2(100, 0))) { if (Save()) DoOpen(m_PendingOpen); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100, 0))) { Revert(); DoOpen(m_PendingOpen); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) { m_PendingOpen.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::End();
}
