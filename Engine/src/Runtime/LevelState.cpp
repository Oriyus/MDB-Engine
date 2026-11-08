#include "Runtime/LevelState.h"
#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Components.h"
#include "Scene/EntityJson.h"
#include "Scene/SceneSerializer.h"
#include "Core/Log.h"

namespace Engine {

    using json = nlohmann::json;

    json EntityState(Scene& scene, entt::entity handle, const ResourceRegistry& resources) {
        Entity e(handle, &scene);
        json j;
        j["name"] = e.Get<TagComponent>().Tag;
        j["id"] = scene.GetID(handle);
        if (e.Has<ParentComponent>()) j["parent"] = e.Get<ParentComponent>().Parent;
        j["transform"] = TransformToJson(e.Get<TransformComponent>());
        if (e.Has<PrefabComponent>()) {
            const auto& pc = e.Get<PrefabComponent>();
            j["prefab"] = pc.Path;
            if (pc.Node != 0) j["node"] = pc.Node;
        }
        j.update(ComponentsToJson(e, resources));
        return j;
    }

    entt::entity ApplyEntityState(Scene& scene, const json& j, const ResourceRegistry& resources) {
        const uint64_t id = j.at("id").get<uint64_t>();
        const std::string name = j.at("name").get<std::string>();
        entt::entity h = scene.FindByID(id);
        Entity e;
        if (h == entt::null) {
            e = scene.CreateEntity(name);
            e.Add<IDComponent>(IDComponent{ id });
        }
        else {
            e = Entity(h, &scene);
            ClearComponents(e);
            e.Get<TagComponent>().Tag = name;
        }
        if (j.contains("transform")) TransformFromJson(j["transform"], e.Get<TransformComponent>());
        if (j.contains("parent")) e.Add<ParentComponent>(ParentComponent{ j["parent"].get<uint64_t>() });
        else if (e.Has<ParentComponent>()) e.Remove<ParentComponent>();
        if (j.contains("prefab")) e.Add<PrefabComponent>(PrefabComponent{ j["prefab"].get<std::string>(), j.value("node", uint64_t(0)) });
        else if (e.Has<PrefabComponent>()) e.Remove<PrefabComponent>();
        ComponentsFromJson(j, e, resources);
        return e.GetHandle();
    }

    bool ReadLevelBase(const std::string& path, const ResourceRegistry& resources, PrefabLibrary* prefabs, LevelBase& out) {
        out.clear();
        Scene scratch;
        if (!SceneSerializer(scratch, resources, prefabs).Load(path, nullptr)) return false;
        for (auto h : scratch.Registry().view<TagComponent>())
            out[scratch.GetID(h)] = EntityState(scratch, h, resources).dump();
        return true;
    }

    std::vector<entt::entity> Subtree(Scene& scene, entt::entity e) {
        std::vector<entt::entity> out{ e };
        for (size_t i = 0; i < out.size(); i++)
            for (entt::entity c : scene.GetChildren(out[i])) out.push_back(c);
        return out;
    }

    json DiffLevel(Scene& scene, const LevelBase& base, const ResourceRegistry& resources,
        const std::unordered_set<uint64_t>& exclude) {
        auto& reg = scene.Registry();

        // Absent: transient entities, excluded ones, and everything under either
        std::unordered_set<entt::entity> absent;
        for (auto h : reg.view<TagComponent>()) {
            if (absent.contains(h)) continue;
            if (reg.all_of<TransientComponent>(h) || exclude.contains(scene.GetID(h)))
                for (entt::entity x : Subtree(scene, h)) absent.insert(x);
        }

        json entities = json::array(), removed = json::array();
        std::unordered_set<uint64_t> present;
        for (auto h : reg.view<TagComponent>()) {
            if (absent.contains(h)) continue;
            const uint64_t id = scene.GetID(h);
            present.insert(id);
            json state = EntityState(scene, h, resources);
            auto it = base.find(id);
            if (it == base.end() || it->second != state.dump()) entities.push_back(std::move(state));
        }
        for (const auto& [id, text] : base)
            if (!present.contains(id)) removed.push_back(id);
        return { { "entities", std::move(entities) }, { "removed", std::move(removed) } };
    }

    void ApplyLevelDiff(Scene& scene, const json& diff, const ResourceRegistry& resources) {
        // Changed and added entities first (they may move children away from a removed parent),
        // then the removals (DestroyEntity takes whatever is still below them)
        if (auto it = diff.find("entities"); it != diff.end())
            for (const json& j : *it) ApplyEntityState(scene, j, resources);
        if (auto it = diff.find("removed"); it != diff.end())
            for (const json& id : *it) {
                const entt::entity h = scene.FindByID(id.get<uint64_t>());
                if (h != entt::null) scene.DestroyEntity(Entity(h, &scene));
            }
    }

} // namespace Engine
