#pragma once
#include "ModelPreview.h"
#include "Renderer/ShaderGraph.h"

#include <nlohmann/json.hpp>
#include <imgui.h>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace Engine { class ResourceLibrary; }

// The Shader Graph Editor (Window > Shader Graph, or double-click a .shadergraph in Assets): designs a surface as a
// node graph (Renderer/ShaderGraph.h), like Unity's Shader Graph, Amplify Shader or Unreal's material editor.
//
// Canvas: nodes wired output -> input into the Output node. Right-click (or Space) adds a node from a searchable
// list; dragging from a pin and letting go on empty space adds one already connected. Drag a node to move it, drag
// on empty space to select several, middle / right drag or Alt + drag pans, the wheel zooms, F frames everything.
// Alt+click or right-click on an input pin disconnects it. Delete, Ctrl+D (duplicate), Ctrl+Z / Ctrl+Y, Ctrl+S.
// Left panel: a live preview (the graph on a sphere / cube / capsule), the params (the values materials set; drag
// one onto the canvas for its node), the graph's settings, the selected node's values, and the problems (graph
// errors per node, GLSL errors, the generated code).
//
// Every change goes live (ResourceLibrary::SetShaderGraph): materials using the graph change at once, also while
// playing. Save writes the file.
class ShaderGraphEditor {
public:
    ShaderGraphEditor(std::string engineDir, std::string assetDir);

    void SetResources(Engine::ResourceLibrary* resources) { m_Resources = resources; }
    void Open(const std::string& path);           // asks first if there are unsaved changes
    void Reopen(const std::string& path) { DoOpen(path); }   // the file (moved or rewritten), unsaved changes dropped
    void New(const std::string& path);            // writes a starter graph there, then opens it
    // Presets: the engine's graphs in EngineAssets/ShaderGraphs/Presets (names = file stems, "Water")
    std::vector<std::string> Presets() const;
    void LoadPreset(const std::string& name);    // replaces the open graph (an undo step)
    void NewFromPreset(const std::string& name); // writes Shaders/<name>.shadergraph (made unique) and opens it
    const std::string& Path() const { return m_Path; }
    void Draw(bool* open);
    void Render(float dt);                        // the preview (outside the ImGui frame)
    void ReloadShaders() { m_Preview.ReloadShaders(); }

    bool IsFocused() const { return m_Focused; }
    bool IsDirty() const;
    bool Save();
    void Revert();
    bool Holds(const std::string& path) const { return IsDirty() && path == m_Path; }

    std::function<uint32_t(const std::string& path)> TextureId;   // image thumbnails

private:
    // canvas
    struct PinRef { int Node = 0; std::string Pin; bool Output = false; int Type = 1; };
    ImVec2 ToScreen(const glm::vec2& p) const;
    glm::vec2 ToCanvas(const ImVec2& p) const;
    float NodeWidth(const Engine::SGNode& n) const;
    float NodeHeight(const Engine::SGNode& n) const;     // canvas units, with its thumbnail
    bool ShowsPreview(const Engine::SGNode& n) const;
    ImVec2 PinPos(const Engine::SGNode& n, int index, bool output) const;
    bool PinAt(const ImVec2& mouse, PinRef& out) const;
    int NodeAt(const ImVec2& mouse) const;
    void DrawCanvas();
    void DrawNode(ImDrawList* dl, const Engine::SGNode& n);
    void DrawAddMenu();
    void FrameAll();

    // panels
    void DrawSidePanel();
    void DrawParams();
    void DrawNodeProperties();
    void DrawProblems();
    bool ImagePicker(const char* label, std::string& path);
    bool ValueEditor(const char* label, nlohmann::json& value, int dims);

    // edits
    int AddNode(const std::string& type, const glm::vec2& pos);
    void Connect(const PinRef& a, const PinRef& b);
    void Disconnect(int node, const std::string& inputPin);
    void DeleteSelected();
    void DuplicateSelected();
    void Changed();                               // after any edit: publish if it compiles differently
    void Publish();
    void Commit();
    void Undo();
    void Redo();
    void DoOpen(const std::string& path);
    std::string UniqueParamName(const std::string& base) const;

    std::string m_EngineDir, m_AssetDir;
    Engine::ResourceLibrary* m_Resources = nullptr;
    std::string m_Path;
    Engine::ShaderGraph m_Graph;
    std::string m_SavedText, m_PublishedText;
    std::vector<std::string> m_Undo;
    int m_UndoPos = -1;
    Engine::SGCompiled m_Compiled;
    std::string m_GlslError;
    bool m_Focused = false;
    std::string m_Status;

    // view and interaction
    ImVec2 m_Origin{ 0, 0 }, m_Size{ 0, 0 };      // the canvas rectangle on screen
    glm::vec2 m_Pan{ 0.0f };
    float m_Zoom = 1.0f;
    int m_FrameNext = 3;                          // frame everything over the next few draws (the window may still be docking)
    std::set<int> m_Selected;
    int m_Dragging = 0;                           // a node being moved (the selection moves with it)
    bool m_Panning = false, m_BoxSelecting = false;
    ImVec2 m_BoxStart{ 0, 0 };
    bool m_Linking = false;
    PinRef m_LinkFrom;
    bool m_OpenAddMenu = false;
    glm::vec2 m_AddAt{ 0.0f };
    bool m_AddConnect = false;                    // the node added from the menu gets m_LinkFrom connected
    char m_Search[64] = {};
    bool m_ShowCode = false;
    float m_SideWidth = 300.0f;

    std::string m_PendingOpen;
    bool m_AskSwitch = false;

    ModelPreview m_Preview;
    Engine::SGNodePreviews m_Previews;            // the nodes' thumbnails
    bool m_ShowPreviews = true;
    bool m_Visible = false;                       // drawn last frame (thumbnails are only rendered then)
    float m_Time = 0.0f;                          // their clock (Time nodes)
    std::vector<std::string> m_Images;
    double m_LastScan = -10.0;
};
