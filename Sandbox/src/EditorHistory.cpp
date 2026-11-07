#include "EditorHistory.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/EntityJson.h"
#include "Scene/SceneSerializer.h"

#include <algorithm>

using namespace Engine;

namespace {

    struct EntityText {
        std::string Name, Transform, Rest;      // Rest: prefab link + components
        std::string All() const { return Name + '\n' + Transform + '\n' + Rest; }
    };

    std::vector<EntityText> Describe(Scene& scene, const ResourceRegistry& res) {
        std::vector<EntityText> out;
        auto& reg = scene.Registry();
        for (auto h : reg.view<TagComponent>()) {
            Entity e(h, &scene);
            EntityText t;
            t.Name = e.Get<TagComponent>().Tag;
            t.Transform = TransformToJson(e.Get<TransformComponent>()).dump();
            nlohmann::json rest = ComponentsToJson(e, res);
            if (e.Has<PrefabComponent>()) rest["prefab"] = e.Get<PrefabComponent>().Path;
            t.Rest = rest.dump();
            out.push_back(std::move(t));
        }
        return out;
    }

    std::string Join(const std::vector<EntityText>& v) {
        std::string s;
        for (const auto& e : v) { s += e.All(); s += "\n\n"; }
        return s;
    }

    // Name a change by what differs between two scenes, e.g. "Add Cube", "Move Coin",
    // "Edit Sun", "Delete 3 entities". Entities are matched as text (a multiset difference).
    std::string Describe(const std::vector<EntityText>& before, const std::vector<EntityText>& after) {
        std::vector<std::string> a, b;
        for (const auto& e : before) a.push_back(e.All());
        for (const auto& e : after) b.push_back(e.All());
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        std::vector<std::string> removed, added;
        std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(removed));
        std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(added));

        auto find = [](const std::vector<EntityText>& v, const std::string& all) -> const EntityText* {
            for (const auto& e : v) if (e.All() == all) return &e;
            return nullptr;
        };
        auto plural = [](size_t n) { return std::to_string(n) + " entities"; };

        if (before.size() < after.size()) {
            const size_t n = after.size() - before.size();
            if (n == 1 && added.size() == 1) return "Add " + find(after, added[0])->Name;
            return "Add " + plural(n);
        }
        if (before.size() > after.size()) {
            const size_t n = before.size() - after.size();
            if (n == 1 && removed.size() == 1) return "Delete " + find(before, removed[0])->Name;
            return "Delete " + plural(n);
        }
        if (added.size() == 1 && removed.size() == 1) {
            const EntityText* was = find(before, removed[0]);
            const EntityText* now = find(after, added[0]);
            if (was->Name != now->Name && was->Transform == now->Transform && was->Rest == now->Rest)
                return "Rename " + was->Name;
            if (was->Rest == now->Rest && was->Name == now->Name) {
                // Only the transform changed: say which part
                const auto a = nlohmann::json::parse(was->Transform), b = nlohmann::json::parse(now->Transform);
                const bool p = a["position"] != b["position"], r = a["rotation"] != b["rotation"], s = a["scale"] != b["scale"];
                if (p && !r && !s) return "Move " + now->Name;
                if (r && !p && !s) return "Rotate " + now->Name;
                if (s && !p && !r) return "Scale " + now->Name;
                return "Transform " + now->Name;
            }
            return "Edit " + now->Name;
        }
        return added.empty() ? "Reorder" : "Edit " + plural(added.size());
    }

    int IndexOf(Scene& scene, entt::entity selected) {
        if (selected == entt::null) return -1;
        int i = 0;
        for (auto h : scene.Registry().view<TagComponent>()) {
            if (h == selected) return i;
            i++;
        }
        return -1;
    }

    const std::string kNothing;

} // namespace

std::string EditorHistory::MakeKey(Scene& scene, const ResourceRegistry& res) {
    return Join(::Describe(scene, res));
}

void EditorHistory::Reset(Scene& scene, entt::entity selected, const ResourceRegistry& res) {
    m_States.clear();
    State s;
    int unused = -1;
    s.Snapshot = scene.Capture(entt::null, unused);
    s.Selected = IndexOf(scene, selected);
    s.Key = MakeKey(scene, res);
    m_States.push_back(std::move(s));
    m_Index = 0;
    m_Saved = 0;
}

bool EditorHistory::Track(Scene& scene, entt::entity selected, const ResourceRegistry& res, const std::string& givenLabel) {
    if (m_States.empty()) { Reset(scene, selected, res); return false; }

    const auto now = ::Describe(scene, res);
    std::string key = Join(now);
    if (key == m_States[m_Index].Key) {
        m_States[m_Index].Selected = IndexOf(scene, selected);   // keep the latest selection for this state
        return false;
    }

    // Describe the step against the current state (re-derived from its snapshot)
    std::string label = givenLabel;
    if (label.empty()) {
        Scene before;
        before.Restore(m_States[m_Index].Snapshot, -1);
        label = ::Describe(::Describe(before, res), now);
    }

    m_States.resize(m_Index + 1);                 // a new edit drops the redo branch
    if (m_Saved > (long)m_Index) m_Saved = -1;    // the saved state was on that branch

    State s;
    int unused = -1;
    s.Snapshot = scene.Capture(entt::null, unused);
    s.Selected = IndexOf(scene, selected);
    s.Key = std::move(key);
    s.Label = std::move(label);
    m_States.push_back(std::move(s));
    m_Index++;

    if (m_States.size() > m_Limit) {              // forget the oldest
        m_States.erase(m_States.begin());
        m_Index--;
        m_Saved = m_Saved > 0 ? m_Saved - 1 : -1;
    }
    return true;
}

const std::string& EditorHistory::UndoLabel() const { return CanUndo() ? m_States[m_Index].Label : kNothing; }
const std::string& EditorHistory::RedoLabel() const { return CanRedo() ? m_States[m_Index + 1].Label : kNothing; }

entt::entity EditorHistory::Undo(Scene& scene) {
    if (!CanUndo()) return entt::null;
    m_Index--;
    return scene.Restore(m_States[m_Index].Snapshot, m_States[m_Index].Selected);   // its selection, as it was
}

entt::entity EditorHistory::Redo(Scene& scene) {
    if (!CanRedo()) return entt::null;
    m_Index++;
    return scene.Restore(m_States[m_Index].Snapshot, m_States[m_Index].Selected);
}
