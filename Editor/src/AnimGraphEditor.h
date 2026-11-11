#pragma once
#include "Animation/AnimGraph.h"
#include <imgui.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Engine { struct AnimatorComponent; }

// The AnimGraph Editor window (Window > AnimGraph Editor, or double-click a graph in Assets/AnimGraphs): an
// animation graph (Engine::AnimGraph) as a state machine per layer, on the same kind of canvas as the Dialogue
// Editor. Left: layers and parameters. Centre: the selected layer's states, the Entry and Any State nodes, and
// transitions as arrows. Right: the selected state's or transition's properties, and a problems list.
//
// Canvas: drag a node's pin (right edge) onto a state to make a transition (from Entry: set the entry state).
// Click an arrow to select it. Right-click: add a state / node menu / transition menu. Double-click empty space:
// a new state. Wheel zooms, right or middle drag pans, F frames everything. Ctrl+S, Ctrl+Z / Ctrl+Y, Delete,
// Ctrl+D while the window has focus.
//
// Live: the editor watches one Animator using this graph file (the selected entity's, else the first one): its
// current states light up, with their progress, the transition being taken and the parameters' values, which
// can be changed (or triggers fired) from the Parameters list. Every edit is applied to the animators using the
// file at once (OnChanged), even before it is saved, so a graph can be tuned while the game runs.
class AnimGraphEditor {
public:
    explicit AnimGraphEditor(std::string assetDir);

    void Draw(bool* open);
    void Open(const std::string& path);       // "AnimGraphs/fox.json" (asks first if there are unsaved changes)
    void New();
    bool IsFocused() const { return m_Focused; }
    bool IsDirty() const;
    const std::string& Path() const { return m_Path; }
    bool SaveIfNamed();

    // The Animator to show live (null: none), its entity's name, and the clips and bones of its model (or of the
    // selected entity's model) for the pickers. Called by the editor every frame before Draw.
    void SetLive(Engine::AnimatorComponent* animator, const std::string& entityName);
    void SetModel(const std::vector<std::string>& clips, const std::vector<std::string>& bones);

    // Called with a copy of the graph whenever it changes (and when one is opened): the app makes it what every
    // Animator using `path` plays
    std::function<void(const std::string& path, std::shared_ptr<const Engine::AnimGraph>)> OnChanged;

private:
    enum class Sel { None, State, Transition, Entry, Any };
    struct Geo { ImVec2 Min, Max, Out; };

    // file
    bool Load(const std::string& path);
    bool SaveTo(const std::string& path);
    std::vector<std::string> ListFiles() const;
    void Publish();                            // OnChanged with the current graph, if it has a file

    // graph helpers
    Engine::AnimGraphLayer& Layer();
    Engine::AnimState& AddState(Engine::AnimStateKind kind, ImVec2 pos, const std::string& clip = "");
    void RemoveState(int id);
    void RemoveTransition(int index);
    void RenameParam(const std::string& from, const std::string& to);
    std::string UniqueStateName(const std::string& base) const;
    std::string UniqueParamName(const std::string& base) const;

    // undo
    void Commit();
    void Undo();
    void Redo();
    void ResetHistory();

    // drawing
    void DrawMenuBar();
    void DrawSidePanel();
    void DrawCanvas();
    void DrawProperties();
    void DrawStateProperties(Engine::AnimState& s);
    void DrawTransitionProperties(int index);
    void DrawPopups();
    Geo StateGeo(const Engine::AnimState& s) const;
    Geo PillGeo(glm::vec2 pos) const;
    ImVec2 ToScreen(ImVec2 canvas) const;
    ImVec2 ToCanvas(ImVec2 screen) const;
    int StateAt(ImVec2 screen) const;
    int TransitionAt(ImVec2 screen) const;
    bool ArrowEnds(const Engine::AnimTransition& t, ImVec2& a, ImVec2& b) const;
    void FrameAll();
    bool InputString(const char* label, std::string& s);
    bool ClipCombo(const char* label, std::string& clip);
    bool ParamCombo(const char* label, std::string& param, bool numbersOnly, const char* none = nullptr);

    std::string m_AssetDir;
    std::string m_Path;
    Engine::AnimGraph m_Graph;
    int m_Layer = 0;
    std::string m_SavedText, m_PublishedText;

    std::vector<std::string> m_History;
    int m_HistoryPos = -1;
    bool m_WantCommit = false;

    // live link and model info (set every frame by the app)
    Engine::AnimatorComponent* m_Live = nullptr;
    std::string m_LiveName;
    std::vector<std::string> m_Clips, m_Bones;

    // view and interaction
    ImVec2 m_Pan{ 40, 40 };
    float m_Zoom = 1.0f;
    ImVec2 m_CanvasMin{ 0, 0 }, m_CanvasSize{ 0, 0 }, m_LastCanvasSize{ -1, -1 };
    bool m_Focused = false, m_FrameNext = false;
    Sel m_Sel = Sel::None;
    int m_SelState = 0, m_SelTransition = -1;
    Sel m_Drag = Sel::None;                    // a node being moved (state, Entry or Any)
    int m_DragState = 0;
    bool m_DragMoved = false;
    Sel m_LinkFrom = Sel::None;                // dragging a new transition out of a pin
    int m_LinkState = 0;
    int m_ContextState = 0, m_ContextTransition = -1;
    ImVec2 m_PopupPos{ 0, 0 };
    bool m_RightDragged = false;

    // dialogs
    std::string m_PendingOpen;
    bool m_PendingNew = false, m_AskUnsaved = false, m_AskSaveAs = false;
    char m_SaveAsName[64] = {};
    std::string m_Status;
};
