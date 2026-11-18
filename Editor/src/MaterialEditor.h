#pragma once
#include "ModelPreview.h"

#include <nlohmann/json.hpp>
#include <imgui.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Engine { class ResourceLibrary; struct MaterialAsset; struct MaterialDesc; }

// The Material Editor (Window > Material Editor, or double-click a .mat file in Assets): one material file at a time
// (Renderer/MaterialAsset.h). A preview on a sphere / cube / capsule on top, the values below: surface colour and
// map, transparency, metal / roughness (a packed map or grey ones), normal map, emission, height map (terrain
// layers blend by it), texture placement. A variant (a material with a parent) shows which values it changes: the
// dot before a value puts the parent's back.
//
// Every edit goes live at once (ResourceLibrary::SetMaterialFile), so every part and terrain layer using the file
// changes, also while playing. Save writes the file; Revert reads it again. Its own undo (Ctrl+Z / Ctrl+Y) and Ctrl+S
// while the window has focus. Opening another material with unsaved changes asks first.
class MaterialEditor {
public:
    MaterialEditor(std::string engineDir, std::string assetDir);

    void SetResources(Engine::ResourceLibrary* resources) { m_Resources = resources; }
    void Open(const std::string& path);           // asks first if the open one has unsaved changes
    void Reopen(const std::string& path) { DoOpen(path); }   // the file (moved or rewritten), unsaved changes dropped
    const std::string& Path() const { return m_Path; }
    void Draw(bool* open);
    void Render(float dt);                        // the preview (outside the ImGui frame, from OnUpdate)
    void ReloadShaders() { m_Preview.ReloadShaders(); }

    bool IsFocused() const { return m_Focused; }
    bool IsDirty() const;
    bool Save();
    void Revert();                                // the file again (unsaved changes are lost)
    // RefreshMaterials must not read this file again while it has unsaved changes here
    bool Holds(const Engine::MaterialAsset& m) const;

    std::function<uint32_t(const std::string& path)> TextureId;   // map thumbnails (GL texture, 0 = none)
    std::function<void(const std::string& graphPath)> OnEditGraph;   // "Edit graph": open it in the Shader Graph Editor

private:
    void Apply();                                 // m_File -> the live material
    void Commit();                                // an undo step, when nothing is held
    void Undo();
    void Redo();
    void DoOpen(const std::string& path);

    // One value row: edit changes a copy of the resolved values; the keys it touched are written to the file
    bool Value(const char* key, const std::function<bool(Engine::MaterialDesc&)>& edit, const char* key2 = nullptr);
    bool MapPicker(const char* label, std::string& path, const char* tip);
    bool MaterialPicker(const char* label, std::string& path);
    void ScanAssets();
    void DrawGraphMaterial();                     // a shader graph material: the graph's params, the render state

    std::string m_EngineDir, m_AssetDir;
    Engine::ResourceLibrary* m_Resources = nullptr;
    std::shared_ptr<Engine::MaterialAsset> m_Asset;
    std::string m_Path;
    nlohmann::json m_File = nlohmann::json::object();     // the file's keys being edited
    nlohmann::json m_Saved = nlohmann::json::object();    // as on disk
    std::vector<nlohmann::json> m_Undo;
    int m_UndoPos = -1;
    bool m_Focused = false;
    std::string m_PendingOpen;                    // waiting for the unsaved-changes answer
    bool m_AskSwitch = false;
    std::string m_Status;

    ModelPreview m_Preview;

    std::vector<std::string> m_Images, m_Materials, m_Graphs;   // under Assets, for the pickers
    double m_LastScan = -10.0;
};
