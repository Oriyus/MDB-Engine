#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/EntityJson.h"
#include "Scene/SceneSerializer.h"
#include "Core/Log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace Engine {

    // ---------------------------------------------------------------- overrides as JSON
    json DiffComponents(const json& prefab, const json& instance) {
        json out = json::object();
        for (auto it = instance.begin(); it != instance.end(); ++it) {
            if (!prefab.contains(it.key())) { out[it.key()] = it.value(); continue; }   // added
            const json& base = prefab[it.key()];
            if (!it.value().is_object() || !base.is_object()) {
                if (it.value() != base) out[it.key()] = it.value();
                continue;
            }
            json fields = json::object();
            for (auto f = it.value().begin(); f != it.value().end(); ++f)
                if (!base.contains(f.key()) || base[f.key()] != f.value()) fields[f.key()] = f.value();
            if (!fields.empty()) out[it.key()] = std::move(fields);
        }
        for (auto it = prefab.begin(); it != prefab.end(); ++it)
            if (!instance.contains(it.key())) out[it.key()] = nullptr;                    // removed
        return out;
    }

    json MergeOverrides(const json& prefab, const json& overrides) {
        json out = prefab;
        if (!overrides.is_object()) return out;
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
            if (it.value().is_null()) { out.erase(it.key()); continue; }
            if (!out.contains(it.key()) || !it.value().is_object() || !out[it.key()].is_object()) {
                out[it.key()] = it.value();
                continue;
            }
            for (auto f = it.value().begin(); f != it.value().end(); ++f) out[it.key()][f.key()] = f.value();
        }
        return out;
    }

    namespace {

        uint64_t NodeOf(Entity e) { return e.Has<PrefabComponent>() ? e.Get<PrefabComponent>().Node : 0; }

        bool LinkedTo(Entity e, const std::string& path) {
            return e.IsValid() && e.Has<PrefabComponent>() && e.Get<PrefabComponent>().Path == path;
        }

        // The node with this key under (and including) `node`; key 0 is the root
        const json* FindNode(const json& node, uint64_t key) {
            if (key == 0) return &node;
            if (!node.contains("children")) return nullptr;
            for (const auto& c : node["children"]) {
                if (c.value("key", uint64_t(0)) == key) return &c;
                if (const json* found = FindNode(c, key)) return found;
            }
            return nullptr;
        }

        void CollectKeys(const json& node, std::vector<uint64_t>& out) {
            if (!node.contains("children")) return;
            for (const auto& c : node["children"]) {
                out.push_back(c.value("key", uint64_t(0)));
                CollectKeys(c, out);
            }
        }

        // An instance's linked parts: the root and every descendant linked to the same prefab
        void CollectParts(Scene& scene, entt::entity h, const std::string& path, std::vector<entt::entity>& out) {
            if (LinkedTo(Entity(h, &scene), path)) out.push_back(h);
            for (auto c : scene.GetChildren(h)) CollectParts(scene, c, path, out);
        }

    } // namespace

    struct PrefabLibrary::Impl {
        std::string AssetDir;
        ResourceRegistry Resources;

        struct Cached {
            json File;                             // the whole prefab file, normalised
            fs::file_time_type Time{};
        };
        std::unordered_map<std::string, Cached> Cache;
        std::unordered_set<std::string> Warned;    // report a broken prefab once, not every frame

        // Canonical form: every node's components and transform go through a scratch entity and
        // back. A hand-edited "0.2" becomes the float the engine actually stores, so parts compare
        // equal to it and overrides only list real differences.
        void Normalise(json& node, Scene& scratch) {
            Entity probe = scratch.CreateEntity();
            ComponentsFromJson(node["components"], probe, Resources);
            node["components"] = ComponentsToJson(probe, Resources);
            if (node.contains("transform")) {
                TransformComponent t;
                TransformFromJson(node["transform"], t);
                node["transform"] = TransformToJson(t);
            }
            scratch.DestroyEntity(probe);
            if (node.contains("children"))
                for (auto& c : node["children"]) {
                    if (!c.contains("components")) c["components"] = json::object();
                    Normalise(c, scratch);
                }
        }

        // The parsed prefab, re-read if the file changed. Null if missing or malformed.
        const json* Get(const std::string& path) {
            std::error_code ec;
            const fs::path full = fs::path(AssetDir) / path;
            const auto time = fs::last_write_time(full, ec);
            if (ec) {
                if (Warned.insert(path).second) ENGINE_WARN("Prefab not found: {}", path);
                Cache.erase(path);
                return nullptr;
            }
            auto it = Cache.find(path);
            if (it != Cache.end() && it->second.Time == time) return &it->second.File;

            try {
                std::ifstream in(full);
                json j;
                in >> j;
                if (!j.contains("components") || !j["components"].is_object())
                    throw std::runtime_error("no \"components\" object");
                Scene scratch;
                Normalise(j, scratch);
                Warned.erase(path);
                return &(Cache[path] = Cached{ std::move(j), time }).File;
            }
            catch (const std::exception& ex) {
                if (Warned.insert(path).second) ENGINE_ERROR("Prefab {} is malformed: {}", path, ex.what());
                Cache.erase(path);
                return nullptr;
            }
        }

        const json* Node(const std::string& path, uint64_t key) {
            const json* p = Get(path);
            return p ? FindNode(*p, key) : nullptr;
        }

        bool Write(const std::string& path, const json& j) {
            std::error_code ec;
            const fs::path full = fs::path(AssetDir) / path;
            fs::create_directories(full.parent_path(), ec);
            std::ofstream out(full);
            if (!out) { ENGINE_ERROR("Could not write prefab: {}", full.string()); return false; }
            out << j.dump(2);
            out.close();
            Cache.erase(path);                     // re-read (and re-timestamp) on next use
            Warned.erase(path);
            return true;
        }

        // A part = its node's components with the part's overrides laid on top
        bool Fill(Entity e, const std::string& path, uint64_t key, const json& overrides, bool takeTransform) {
            const json* n = Node(path, key);
            if (!n) return false;
            try {
                const json merged = MergeOverrides((*n)["components"], overrides);
                ClearComponents(e);
                ComponentsFromJson(merged, e, Resources);
                if (takeTransform && key != 0 && n->contains("transform"))
                    TransformFromJson((*n)["transform"], e.Get<TransformComponent>());
            }
            catch (const std::exception& ex) {
                ENGINE_ERROR("Prefab {} (or an instance's overrides) has a bad component: {}", path, ex.what());
                return false;
            }
            e.Add<PrefabComponent>(PrefabComponent{ path, key });
            return true;
        }

        json OverridesOf(Entity e) {
            if (!e.Has<PrefabComponent>()) return json::object();
            const auto& pc = e.Get<PrefabComponent>();
            const json* n = Node(pc.Path, pc.Node);
            if (!n) return json::object();
            return DiffComponents((*n)["components"], ComponentsToJson(e, Resources));
        }

        bool TransformOverridden(Entity e) {
            if (!e.Has<PrefabComponent>() || e.Get<PrefabComponent>().Node == 0) return false;   // roots are placed freely
            const auto& pc = e.Get<PrefabComponent>();
            const json* n = Node(pc.Path, pc.Node);
            if (!n || !n->contains("transform")) return false;
            return TransformToJson(e.Get<TransformComponent>()) != (*n)["transform"];
        }

        // A new entity (and its subtree) for a node, under `parent`
        Entity Build(Scene& scene, const std::string& path, const json& node, uint64_t key, entt::entity parent) {
            Entity e = scene.CreateEntity(node.value("name", fs::path(path).stem().string()));
            if (parent != entt::null) e.Add<ParentComponent>(ParentComponent{ scene.GetID(parent) });
            if (node.contains("transform")) {
                try { TransformFromJson(node["transform"], e.Get<TransformComponent>()); }
                catch (const std::exception&) {}   // keep the default transform
            }
            if (!Fill(e, path, key, json::object(), false)) { scene.DestroyEntity(e); return {}; }
            const json children = node.contains("children") ? node["children"] : json::array();   // copy: Fill may re-read
            for (const auto& c : children) Build(scene, path, c, c.value("key", uint64_t(0)), e.GetHandle());
            return e;
        }

        // The prefab tree for an entity and its descendants. Parts already linked to this prefab
        // keep their node keys, so Apply doesn't break other instances' links.
        json BuildNode(Scene& scene, entt::entity h, const std::string& path, bool isRoot,
            std::unordered_map<entt::entity, uint64_t>& keys) {
            Entity e(h, &scene);
            TransformComponent t = e.Get<TransformComponent>();
            if (isRoot) t.Position = glm::vec3(0.0f);                // instances are placed freely
            json n = { { "name", e.Get<TagComponent>().Tag }, { "transform", TransformToJson(t) },
                       { "components", ComponentsToJson(e, Resources) } };
            if (!isRoot) {
                const uint64_t key = (LinkedTo(e, path) && NodeOf(e) != 0) ? NodeOf(e) : Scene::NewID();
                n["key"] = key;
                keys[h] = key;
            }
            json children = json::array();
            for (auto c : scene.GetChildren(h)) children.push_back(BuildNode(scene, c, path, false, keys));
            if (!children.empty()) n["children"] = std::move(children);
            return n;
        }
    };

    PrefabLibrary::PrefabLibrary(std::string assetDir, const ResourceRegistry& resources)
        : m_Impl(std::make_unique<Impl>()) {
        m_Impl->AssetDir = std::move(assetDir);
        m_Impl->Resources = resources;
    }

    PrefabLibrary::~PrefabLibrary() = default;

    std::string PrefabLibrary::Resolve(const std::string& name) {
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return name;
        if (fs::path(name).extension() == ".json") return "Prefabs/" + name;
        return "Prefabs/" + name + ".json";
    }

    bool PrefabLibrary::Exists(const std::string& path) const {
        std::error_code ec;
        return fs::exists(fs::path(m_Impl->AssetDir) / path, ec);
    }

    std::vector<std::string> PrefabLibrary::List() const {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(fs::path(m_Impl->AssetDir) / "Prefabs", ec))
            if (f.path().extension() == ".json") out.push_back("Prefabs/" + f.path().filename().string());
        std::sort(out.begin(), out.end());
        return out;
    }

    bool PrefabLibrary::CanLoad(const std::string& path) { return m_Impl->Get(path) != nullptr; }
    bool PrefabLibrary::HasNode(const std::string& path, uint64_t node) { return m_Impl->Node(path, node) != nullptr; }

    Entity PrefabLibrary::Instantiate(Scene& scene, const std::string& path) {
        const json* p = m_Impl->Get(path);
        if (!p) return {};
        const json root = *p;                      // a copy: building may re-read the cache
        Entity e = m_Impl->Build(scene, path, root, 0, entt::null);
        if (e) e.Get<TransformComponent>().Position = glm::vec3(0.0f);
        return e;
    }

    bool PrefabLibrary::ApplyPrefabTo(Entity e, const std::string& path, uint64_t node, const json& overrides,
        bool takeNodeTransform) {
        return m_Impl->Fill(e, path, node, overrides, takeNodeTransform);
    }

    std::vector<uint64_t> PrefabLibrary::MissingNodes(Scene& scene, Entity root) {
        std::vector<uint64_t> missing;
        if (!root.Has<PrefabComponent>()) return missing;
        const std::string path = root.Get<PrefabComponent>().Path;
        const json* p = m_Impl->Get(path);
        if (!p) return missing;
        std::vector<entt::entity> parts;
        CollectParts(scene, root.GetHandle(), path, parts);
        std::unordered_set<uint64_t> have;
        for (auto h : parts) have.insert(NodeOf(Entity(h, &scene)));
        std::vector<uint64_t> all;
        CollectKeys(*p, all);
        for (auto k : all) if (!have.count(k)) missing.push_back(k);
        return missing;
    }

    void PrefabLibrary::SyncInstance(Scene& scene, Entity root, const std::vector<uint64_t>& skip) {
        if (!root.Has<PrefabComponent>() || root.Get<PrefabComponent>().Node != 0) return;
        const std::string path = root.Get<PrefabComponent>().Path;
        const json* p = m_Impl->Get(path);
        if (!p) return;
        const json tree = *p;

        std::vector<entt::entity> parts;
        CollectParts(scene, root.GetHandle(), path, parts);
        std::unordered_map<uint64_t, entt::entity> byNode;
        for (auto h : parts) byNode[NodeOf(Entity(h, &scene))] = h;

        // Parts whose node the prefab no longer has: keep them, as plain entities
        for (auto h : parts) {
            Entity e(h, &scene);
            if (NodeOf(e) != 0 && !FindNode(tree, NodeOf(e))) {
                ENGINE_INFO("'{}' is no longer part of prefab {}: kept as a plain entity", e.Get<TagComponent>().Tag, path);
                e.Remove<PrefabComponent>();
            }
        }

        // Nodes the instance lacks: build them under the right parent part
        const std::unordered_set<uint64_t> skipped(skip.begin(), skip.end());
        std::function<void(const json&, entt::entity)> walk = [&](const json& node, entt::entity parent) {
            if (!node.contains("children")) return;
            for (const auto& c : node["children"]) {
                const uint64_t key = c.value("key", uint64_t(0));
                if (auto it = byNode.find(key); it != byNode.end()) walk(c, it->second);
                else if (!skipped.count(key)) m_Impl->Build(scene, path, c, key, parent);
            }
        };
        walk(tree, root.GetHandle());
    }

    bool PrefabLibrary::Save(Entity e, const std::string& path) {
        Scene& scene = *e.GetScene();
        std::unordered_map<entt::entity, uint64_t> keys;
        json j = m_Impl->BuildNode(scene, e.GetHandle(), path, true, keys);
        j["version"] = 2;
        if (!m_Impl->Write(path, j)) return false;

        // Link the whole group to the file
        e.Add<PrefabComponent>(PrefabComponent{ path, 0 });
        for (auto& [h, key] : keys) {
            Entity part(h, &scene);
            if (part.Has<PrefabComponent>() && part.Get<PrefabComponent>().Path != path)
                ENGINE_INFO("'{}' was an instance of {}; it is now part of {}", part.Get<TagComponent>().Tag,
                    part.Get<PrefabComponent>().Path, path);
            part.Add<PrefabComponent>(PrefabComponent{ path, key });
        }
        ENGINE_INFO("Saved prefab {} ({} entities)", path, keys.size() + 1);
        return true;
    }

    Entity PrefabLibrary::InstanceRoot(Entity part) {
        if (!part.IsValid() || !part.Has<PrefabComponent>()) return {};
        Scene& scene = *part.GetScene();
        const std::string path = part.Get<PrefabComponent>().Path;
        Entity e = part;
        int guard = 0;
        while (NodeOf(e) != 0 && guard++ < 256) {
            const entt::entity p = scene.GetParent(e.GetHandle());
            if (p == entt::null || !LinkedTo(Entity(p, &scene), path)) return {};   // a stray part: no root
            e = Entity(p, &scene);
        }
        return NodeOf(e) == 0 ? e : Entity{};
    }

    int PrefabLibrary::Apply(Scene& scene, Entity part) {
        Entity root = InstanceRoot(part);
        if (!root) { ENGINE_WARN("Apply: this entity's instance has no root"); return -1; }
        const std::string path = root.Get<PrefabComponent>().Path;

        // Every other instance: remember each part's overrides and whether it was moved, plus the
        // child nodes it deleted, all measured against the old prefab
        struct Part { entt::entity H; uint64_t Node; json Overrides; bool KeepTransform; };
        struct Other { entt::entity Root; std::vector<Part> Parts; std::vector<uint64_t> Missing; };
        std::vector<Other> others;
        for (auto [h, pc] : scene.Registry().view<PrefabComponent>().each()) {
            if (h == root.GetHandle() || pc.Path != path || pc.Node != 0) continue;
            Other o{ h, {}, MissingNodes(scene, Entity(h, &scene)) };
            std::vector<entt::entity> parts;
            CollectParts(scene, h, path, parts);
            for (auto ph : parts) {
                Entity pe(ph, &scene);
                o.Parts.push_back({ ph, NodeOf(pe), m_Impl->OverridesOf(pe), m_Impl->TransformOverridden(pe) });
            }
            others.push_back(std::move(o));
        }

        if (!Save(root, path)) return -1;

        for (auto& o : others) {
            for (auto& pt : o.Parts) {
                Entity pe(pt.H, &scene);
                if (!scene.Registry().valid(pt.H)) continue;
                if (m_Impl->Node(path, pt.Node)) m_Impl->Fill(pe, path, pt.Node, pt.Overrides, !pt.KeepTransform);
                // (a part whose node is gone is unlinked by SyncInstance below)
            }
            SyncInstance(scene, Entity(o.Root, &scene), o.Missing);
        }
        ENGINE_INFO("Applied to prefab {}: {} other instance(s) updated", path, others.size());
        return (int)others.size();
    }

    bool PrefabLibrary::Revert(Entity part) {
        if (!part.Has<PrefabComponent>()) return false;
        const auto pc = part.Get<PrefabComponent>();
        return m_Impl->Fill(part, pc.Path, pc.Node, json::object(), true);
    }

    bool PrefabLibrary::IsModified(Entity part) {
        if (!part.Has<PrefabComponent>()) return false;
        const auto& pc = part.Get<PrefabComponent>();
        if (!m_Impl->Node(pc.Path, pc.Node)) return true;
        return !m_Impl->OverridesOf(part).empty() || m_Impl->TransformOverridden(part);
    }

    bool PrefabLibrary::TransformOverridden(Entity part) { return m_Impl->TransformOverridden(part); }

    json PrefabLibrary::GetOverrides(Entity part) { return m_Impl->OverridesOf(part); }

    std::vector<std::string> PrefabLibrary::ListOverrides(Entity part) {
        std::vector<std::string> out;
        if (!part.Has<PrefabComponent>()) return out;
        const auto& pc = part.Get<PrefabComponent>();
        const json* n = m_Impl->Node(pc.Path, pc.Node);
        if (!n) return out;
        if (m_Impl->TransformOverridden(part)) out.push_back("transform");
        const json overrides = m_Impl->OverridesOf(part);
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
            if (it.value().is_null()) out.push_back("-" + it.key());
            else if (!(*n)["components"].contains(it.key())) out.push_back("+" + it.key());
            else for (auto f = it.value().begin(); f != it.value().end(); ++f) out.push_back(it.key() + "." + f.key());
        }
        return out;
    }

    bool PrefabLibrary::RevertOverride(Entity part, const std::string& item) {
        if (!part.Has<PrefabComponent>() || item.empty()) return false;
        const auto pc = part.Get<PrefabComponent>();
        if (item == "transform") {
            const json* n = m_Impl->Node(pc.Path, pc.Node);
            if (!n || pc.Node == 0 || !n->contains("transform")) return false;
            TransformFromJson((*n)["transform"], part.Get<TransformComponent>());
            return true;
        }
        json overrides = m_Impl->OverridesOf(part);
        if (item[0] == '-' || item[0] == '+') {
            overrides.erase(item.substr(1));
        }
        else {
            const size_t dot = item.find('.');
            if (dot == std::string::npos) return false;
            const std::string comp = item.substr(0, dot), field = item.substr(dot + 1);
            if (!overrides.contains(comp) || !overrides[comp].is_object()) return false;
            overrides[comp].erase(field);
            if (overrides[comp].empty()) overrides.erase(comp);
        }
        return m_Impl->Fill(part, pc.Path, pc.Node, overrides, false);
    }

} // namespace Engine
