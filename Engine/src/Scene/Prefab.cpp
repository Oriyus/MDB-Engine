#include "Scene/Prefab.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/EntityJson.h"
#include "Scene/SceneSerializer.h"
#include "Core/Log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace Engine {

    // ---------------------------------------------------------------- overrides as JSON
    // Per component key:
    //   "meshRenderer": { "color": [1,0,0] }   these fields differ from the prefab
    //   "audio": { ...the whole component... }  a component the prefab doesn't have
    //   "spin": null                           the prefab's component is removed
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

    struct PrefabLibrary::Impl {
        std::string AssetDir;
        ResourceRegistry Resources;

        struct Cached {
            json File;                             // the whole prefab file
            fs::file_time_type Time{};
        };
        std::unordered_map<std::string, Cached> Cache;
        std::unordered_set<std::string> Warned;    // report a broken prefab once, not every frame

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

                // Canonical form: load the components into a scratch entity and write them back.
                // A hand-edited "0.2" becomes the float the engine actually stores, so instances
                // compare equal to it, and overrides only list real differences.
                Scene scratch;
                Entity probe = scratch.CreateEntity();
                ComponentsFromJson(j["components"], probe, Resources);
                j["components"] = ComponentsToJson(probe, Resources);

                Warned.erase(path);
                return &(Cache[path] = Cached{ std::move(j), time }).File;
            }
            catch (const std::exception& ex) {
                if (Warned.insert(path).second) ENGINE_ERROR("Prefab {} is malformed: {}", path, ex.what());
                Cache.erase(path);
                return nullptr;
            }
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

        // An instance = the prefab's components with the instance's overrides laid on top
        bool Fill(Entity e, const std::string& path, const json& overrides) {
            const json* p = Get(path);
            if (!p) return false;
            try {
                const json merged = MergeOverrides((*p)["components"], overrides);
                ClearComponents(e);
                ComponentsFromJson(merged, e, Resources);
            }
            catch (const std::exception& ex) {
                ENGINE_ERROR("Prefab {} (or an instance's overrides) has a bad component: {}", path, ex.what());
                return false;
            }
            e.Add<PrefabComponent>(PrefabComponent{ path });
            return true;
        }

        // What an instance changes relative to its prefab ({} if nothing, or if the prefab is missing)
        json OverridesOf(Entity e) {
            if (!e.Has<PrefabComponent>()) return json::object();
            const json* p = Get(e.Get<PrefabComponent>().Path);
            if (!p) return json::object();
            return DiffComponents((*p)["components"], ComponentsToJson(e, Resources));
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

    Entity PrefabLibrary::Instantiate(Scene& scene, const std::string& path) {
        const json* p = m_Impl->Get(path);
        if (!p) return {};
        Entity e = scene.CreateEntity(p->value("name", fs::path(path).stem().string()));
        if (!m_Impl->Fill(e, path, json::object())) { scene.DestroyEntity(e); return {}; }
        if (p->contains("transform")) {
            try {
                auto& t = e.Get<TransformComponent>();
                TransformFromJson((*p)["transform"], t);
                t.Position = glm::vec3(0.0f);
            }
            catch (const std::exception&) {}       // keep the default transform
        }
        return e;
    }

    bool PrefabLibrary::ApplyPrefabTo(Entity e, const std::string& path, const json& overrides) {
        return m_Impl->Fill(e, path, overrides);
    }

    bool PrefabLibrary::Save(Entity e, const std::string& path) {
        TransformComponent defaults = e.Get<TransformComponent>();
        defaults.Position = glm::vec3(0.0f);
        const json j = {
            { "version", 1 },
            { "name", e.Get<TagComponent>().Tag },
            { "transform", TransformToJson(defaults) },
            { "components", ComponentsToJson(e, m_Impl->Resources) },
        };
        if (!m_Impl->Write(path, j)) return false;
        e.Add<PrefabComponent>(PrefabComponent{ path });
        ENGINE_INFO("Saved prefab {}", path);
        return true;
    }

    int PrefabLibrary::Apply(Scene& scene, Entity instance) {
        if (!instance.Has<PrefabComponent>()) return -1;
        const std::string path = instance.Get<PrefabComponent>().Path;

        // Every other instance keeps its own overrides (measured against the old prefab)
        // and gets the new prefab underneath them
        std::vector<std::pair<entt::entity, json>> others;
        for (auto [h, pc] : scene.Registry().view<PrefabComponent>().each())
            if (h != instance.GetHandle() && pc.Path == path) others.emplace_back(h, m_Impl->OverridesOf(Entity(h, &scene)));

        if (!Save(instance, path)) return -1;
        int updated = 0;
        for (auto& [h, overrides] : others)
            if (m_Impl->Fill(Entity(h, &scene), path, overrides)) updated++;
        ENGINE_INFO("Applied to prefab {}: {} other instance(s) updated", path, updated);
        return updated;
    }

    bool PrefabLibrary::Revert(Entity instance) {
        if (!instance.Has<PrefabComponent>()) return false;
        return m_Impl->Fill(instance, instance.Get<PrefabComponent>().Path, json::object());
    }

    bool PrefabLibrary::IsModified(Entity instance) {
        if (!instance.Has<PrefabComponent>()) return false;
        if (!m_Impl->Get(instance.Get<PrefabComponent>().Path)) return true;
        return !m_Impl->OverridesOf(instance).empty();
    }

    json PrefabLibrary::GetOverrides(Entity instance) { return m_Impl->OverridesOf(instance); }

    bool PrefabLibrary::CanLoad(const std::string& path) { return m_Impl->Get(path) != nullptr; }

    std::vector<std::string> PrefabLibrary::ListOverrides(Entity instance) {
        std::vector<std::string> out;
        if (!instance.Has<PrefabComponent>()) return out;
        const json* p = m_Impl->Get(instance.Get<PrefabComponent>().Path);
        if (!p) return out;
        const json overrides = m_Impl->OverridesOf(instance);
        for (auto it = overrides.begin(); it != overrides.end(); ++it) {
            if (it.value().is_null()) out.push_back("-" + it.key());
            else if (!(*p)["components"].contains(it.key())) out.push_back("+" + it.key());
            else for (auto f = it.value().begin(); f != it.value().end(); ++f) out.push_back(it.key() + "." + f.key());
        }
        return out;
    }

    bool PrefabLibrary::RevertOverride(Entity instance, const std::string& item) {
        if (!instance.Has<PrefabComponent>() || item.empty()) return false;
        json overrides = m_Impl->OverridesOf(instance);
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
        return m_Impl->Fill(instance, instance.Get<PrefabComponent>().Path, overrides);
    }

} // namespace Engine
