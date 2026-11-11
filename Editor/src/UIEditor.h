#pragma once
#include "Runtime/UIDocument.h"
#include "Runtime/UIPreview.h"
#include "Runtime/GameUI.h"

#include <imgui.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The UI Editor window (Window > UI Editor, or double-click a file in Assets/UI): designs game UI as layouts
// (Engine::UIDocument, Assets/UI/<name>.json) that a script draws each frame with ui.show("UI/<name>.json").
//
// Left: the widget tree (panels at the top, rows / columns / scroll regions holding labels, buttons, images,
// bars, slots). + adds, right-click for more, drag a row onto a container to put it inside, onto another row to
// put it before. Middle: the layout drawn exactly as the game draws it (same layout code and fonts), at a chosen
// aspect; click a widget to select it, drag to move its panel. Its expressions are evaluated on the document's
// preview values (Preview data tab), and "Try it" lets the buttons run their on_click on those values.
// Right: the selected widget's properties; a number or switch can be turned into a Lua expression (the f button).
//
// Its own undo (Ctrl+Z / Ctrl+Y), Ctrl+S, Ctrl+D and Delete while it has focus. Every change goes live at once
// (OnChanged): a game running in the editor shows the new layout.
class UIEditor {
public:
    static constexpr const char* NodePayload = "MDB_UI_NODE";   // drag and drop in the tree: the node id

    explicit UIEditor(std::string assetDir);

    void Draw(bool* open);
    void Open(const std::string& path);       // "UI/hud.json" (asks first if there are unsaved changes)
    void New();
    bool IsFocused() const { return m_Focused; }
    bool IsDirty() const;
    const std::string& Path() const { return m_Path; }
    bool SaveIfNamed();
    void SelectName(const std::string& name);  // the first widget with that name (framed in the preview)

    void SetFonts(const Engine::UIFonts& fonts) { m_Fonts = fonts; m_UI.SetFonts(fonts.Bold, fonts.Regular); }
    void SetTextures(Engine::UITextureCache* textures) { m_Textures = textures; }

    std::function<void(const std::string& path, std::shared_ptr<const Engine::UIDocument>)> OnChanged;
    std::function<uint32_t(const std::string& path)> TextureId;     // image pickers (GL texture, 0 = none)

private:
    // file
    bool Load(const std::string& path);
    bool SaveTo(const std::string& path);
    std::vector<std::string> ListFiles() const;
    std::string Text() const { return m_Doc.ToJson().dump(); }

    // undo / live
    void Commit();
    void Undo();
    void Redo();
    void ResetHistory();
    void Publish();
    void Restore(const std::string& text);

    // tree
    bool Locate(uint32_t id, std::vector<Engine::UINode>*& list, size_t& index);
    Engine::UINode* ParentOf(uint32_t id);
    bool Contains(const Engine::UINode& root, uint32_t id) const;
    uint32_t TopPanelOf(uint32_t id);
    Engine::UINode MakeNode(const std::string& type);
    void Add(const std::string& type, bool inside);
    void Delete(uint32_t id);
    void Duplicate(uint32_t id);
    void Move(uint32_t id, int dir);
    void MoveTo(uint32_t id, uint32_t target);       // tree drag and drop
    void Renumber(Engine::UINode& n);
    std::string UniqueName(const std::string& base) const;

    // drawing
    void DrawMenuBar();
    void DrawTree();
    void DrawTreeNode(Engine::UINode& n);
    void DrawAddMenu(bool inside);
    void DrawPreview();
    void DrawProperties();
    void DrawWidget(Engine::UINode& n);
    void DrawPreviewData();
    void DrawPopups();

    // property widgets (on the selected node's Props)
    bool PropText(Engine::UINode& n, const char* label, const char* key, bool multiline, const char* help = nullptr);
    bool PropNumber(Engine::UINode& n, const char* label, const char* key, float def, float speed, float lo, float hi, const char* help = nullptr);
    bool PropBool(Engine::UINode& n, const char* label, const char* key, bool def, const char* help = nullptr);
    bool PropExpr(Engine::UINode& n, const char* label, const char* key, const char* hint, const char* help = nullptr);
    bool PropColor(Engine::UINode& n, const char* label, const char* key, ImVec4 def);
    bool PropCombo(Engine::UINode& n, const char* label, const char* key, const std::vector<const char*>& options);
    bool PropImage(Engine::UINode& n, const char* label, const char* key, const char* help = nullptr);
    void Changed() { m_WantCommit = true; }

    std::string m_AssetDir;
    std::string m_Path;                        // "" = not saved yet
    Engine::UIDocument m_Doc;
    std::string m_SavedText, m_PublishedText;
    std::vector<std::pair<std::string, Engine::UIDocument>> m_History;   // (text, document): keeps node ids
    int m_HistoryPos = -1;
    bool m_WantCommit = false;
    std::vector<std::function<void()>> m_Later;   // tree edits asked for while drawing the tree, done after it
    std::string m_IssuesFor;
    std::vector<std::string> m_Issues;

    uint32_t m_Selected = 0, m_Hovered = 0;
    uint32_t m_PendingSelect = 0;              // unfold the tree to it next frame
    bool m_Focused = false;
    std::string m_Status;

    // preview
    Engine::UIFonts m_Fonts;
    Engine::UITextureCache* m_Textures = nullptr;
    Engine::UIContext m_UI;
    Engine::UIPreview m_Preview;
    std::vector<Engine::ScriptUIItem> m_Items;
    std::vector<Engine::UINodeRect> m_Rects;
    std::string m_PreviewError;                // the layout threw (a container mismatch)
    int m_Aspect = 0;                          // index into the aspect list
    bool m_Try = false, m_Outlines = true;
    float m_Zoom = 1.0f;                       // 1 = the view fits the panel; wheel zooms around the pointer
    ImVec2 m_Pan{ 0, 0 };                      // pixels the view is moved from the centre (right / middle drag)
    bool m_FrameSelected = false;              // F: zoom onto the selected widget next frame
    bool m_DragPanel = false, m_DragMoved = false;
    uint32_t m_DragPanelId = 0;
    ImVec2 m_PressPos{ 0, 0 };

    // preview data text (kept in step with m_Doc.Preview)
    std::string m_PreviewText, m_PreviewTextFor, m_PreviewTextError;

    // dialogs
    std::string m_PendingOpen;
    bool m_PendingNew = false, m_AskUnsaved = false, m_AskSaveAs = false;
    char m_SaveAsName[64] = {};
};
