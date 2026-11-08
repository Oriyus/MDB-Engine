#pragma once
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// The Dialogue Editor window: a dialogue tree as a node graph, saved as Assets/Dialogues/<name>.json and
// played in game by Scripts/Lib/dialogue.lua (dialogue.open). The file:
//   { "version": 1, "start": <node id>, "nodes": [ node... ] }
//   line:   { "id", "type": "line", "pos": [x, y], "speaker", "text", "action", "event", "next",
//             "choices": [ { "text", "cond", "show": "hidden" | "disabled", "why", "action", "event", "next" } ] }
//   branch: { "id", "type": "branch", "pos", "cond", "next" (true), "else" (false) }
//   action: { "id", "type": "action", "pos", "action", "event", "next" }
// next / else = 0: the conversation ends there. cond: a Lua expression, action: Lua statements, event: a
// name sent to every script's on_event. A line with choices shows them; without, it continues to next.
//
// Canvas: drag nodes; drag from an output pin (right edge) onto a node to link it, or onto empty space
// to make a new linked node; right-click a pin to unlink. Right-click: add a node / node menu.
// Double-click empty space: a new line. Wheel zooms, right or middle drag pans, F frames everything.
// Ctrl+S saves, Ctrl+Z / Ctrl+Y undo and redo, Delete removes the selected node, Ctrl+D duplicates it.
class DialogueEditor {
public:
    explicit DialogueEditor(std::string assetDir);

    void Draw(bool* open);
    void Open(const std::string& path);       // "Dialogues/sage.json" (asks first if there are unsaved changes)
    void New();
    bool IsFocused() const { return m_Focused; }   // the editor's own shortcuts are off meanwhile
    bool IsDirty() const;
    const std::string& Path() const { return m_Path; }
    void Select(int id) { m_Selected = id; }
    bool SaveIfNamed();                        // Save when the dialogue has a file (used before quitting)

private:
    struct Choice {
        std::string Text, Cond, Show = "hidden", Why, Action, Event;
        int Next = 0;
    };
    struct Node {
        int Id = 0;
        std::string Type = "line";             // line, branch, action
        std::string Speaker, Text, Cond, Action, Event;
        int Next = 0, Else = 0;
        std::vector<Choice> Choices;
        ImVec2 Pos{ 0, 0 };                    // canvas units (pixels at zoom 1)
    };
    struct Geo {                               // a node on screen this frame
        ImVec2 Min, Max, In;
        float BodyTop = 0, BodyBottom = 0;
        std::vector<ImVec2> Outs;
        std::vector<std::string> Labels;
    };

    // file
    nlohmann::json ToJson() const;
    void FromJson(const nlohmann::json& j);
    bool Load(const std::string& path);
    bool SaveTo(const std::string& path);
    std::vector<std::string> ListFiles() const;

    // graph
    Node* Find(int id);
    int NewId() const;
    Node& AddNode(const std::string& type, ImVec2 pos);
    void Remove(int id);
    int* Target(Node& n, int pin);
    int PinCount(const Node& n) const;
    std::vector<std::string> Issues(std::vector<int>* nodes = nullptr);

    // undo
    void Commit();
    void Undo();
    void Redo();
    void ResetHistory();

    // drawing
    void DrawMenuBar();
    void DrawCanvas();
    void DrawProperties();
    void DrawPopups();
    void DrawNodePopups();
    Geo Layout(const Node& n) const;
    ImVec2 ToScreen(ImVec2 canvas) const;
    ImVec2 ToCanvas(ImVec2 screen) const;
    int NodeAt(ImVec2 screen) const;
    bool PinAt(ImVec2 screen, int& node, int& pin) const;
    void FrameAll();
    bool InputString(const char* label, std::string& s, bool multiline = false, float height = 0.0f);

    std::string m_AssetDir;
    std::string m_Path;                        // "" = not saved yet
    int m_Start = 1;
    std::vector<Node> m_Nodes;
    std::string m_SavedText;                   // the JSON as last saved / opened (dirty = differs)

    std::vector<std::string> m_History;
    int m_HistoryPos = -1;
    bool m_WantCommit = false;

    // view and interaction
    ImVec2 m_Pan{ 40, 40 };
    float m_Zoom = 1.0f;
    ImVec2 m_CanvasMin{ 0, 0 }, m_CanvasSize{ 0, 0 }, m_LastCanvasSize{ -1, -1 };
    bool m_Focused = false, m_FrameNext = false;
    int m_Selected = 0;
    int m_DragNode = 0;
    bool m_DragMoved = false;
    int m_LinkNode = 0, m_LinkPin = -1;        // dragging a new link from this pin
    int m_PendingNode = 0, m_PendingPin = -1;  // the "new node" popup links this pin to what it makes
    int m_ContextNode = 0;
    ImVec2 m_PopupPos{ 0, 0 };                 // canvas position for a node made from a popup
    bool m_RightDragged = false;

    // dialogs
    std::string m_PendingOpen;                 // after the unsaved-changes question ("" + m_PendingNew = new)
    bool m_PendingNew = false, m_AskUnsaved = false, m_AskSaveAs = false;
    char m_SaveAsName[64] = {};
    std::string m_Status;
};
