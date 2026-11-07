#pragma once
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"   // ResourceRegistry

#include <cstddef>
#include <string>
#include <vector>

// Undo / redo for the editor, as a list of whole-scene states.
//
// The editor doesn't record each kind of edit. When an interaction ends (mouse released, a
// field loses focus, a key pressed) it calls Track(): if the scene now differs from the current
// state, that becomes a new state, i.e. one undo step. A gizmo drag, a slider drag or typing a name
// is therefore one step, and anything the editor can do to the scene is undoable.
//
// A scene is compared through a text key (every entity's name, transform, prefab link and
// components as JSON), so a state is only recorded when something really changed.
class EditorHistory {
public:
    explicit EditorHistory(size_t limit = 100) : m_Limit(limit) {}

    struct State {
        Engine::SceneSnapshot Snapshot;
        int Selected = -1;          // index of the selected entity in Snapshot (-1: none)
        std::string Key;            // the scene as text, for comparing
        std::string Label;          // what produced this state ("Move Coin", "Delete Lava", ...)
    };

    // Forget everything; `scene` becomes the only state. Counts as saved.
    void Reset(Engine::Scene& scene, entt::entity selected, const Engine::ResourceRegistry& res);

    // Record a new state if the scene changed since the current one. Returns true if it did.
    // `label` names the step (otherwise it is worked out from what changed).
    bool Track(Engine::Scene& scene, entt::entity selected, const Engine::ResourceRegistry& res,
        const std::string& label = "");

    bool CanUndo() const { return m_Index > 0; }
    bool CanRedo() const { return m_Index + 1 < m_States.size(); }
    const std::string& UndoLabel() const;   // the step Undo would take back
    const std::string& RedoLabel() const;

    // Move to the previous / next state and put it in the scene. Returns the entity to select.
    entt::entity Undo(Engine::Scene& scene);
    entt::entity Redo(Engine::Scene& scene);

    void MarkSaved() { m_Saved = (long)m_Index; }
    bool IsDirty() const { return m_Saved != (long)m_Index; }
    size_t Size() const { return m_States.size(); }

    // The scene as text (exposed for tests)
    static std::string MakeKey(Engine::Scene& scene, const Engine::ResourceRegistry& res);

private:
    std::vector<State> m_States;
    size_t m_Index = 0;              // the state the scene is in
    long   m_Saved = 0;              // index of the saved state, -1 if it was dropped from the list
    size_t m_Limit;
};
