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
                // compare equal to it (IsModified) and Apply recognises unmodified instances.
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

        bool Fill(Entity e, const std::string& path) {
            const json* p = Get(path);
            if (!p) return false;
            try {
                ClearComponents(e);
                ComponentsFromJson((*p)["components"], e, Resources);
            }
            catch (const std::exception& ex) {
                ENGINE_ERROR("Prefab {} has a bad component: {}", path, ex.what());
                return false;
            }
            e.Add<PrefabComponent>(PrefabComponent{ path });
            return true;
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
        if (!m_Impl->Fill(e, path)) { scene.DestroyEntity(e); return {}; }
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

    bool PrefabLibrary::ApplyPrefabTo(Entity e, const std::string& path) { return m_Impl->Fill(e, path); }

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

        // Instances that still match the old prefab follow the new one; modified ones keep their copy
        std::vector<entt::entity> followers;
        if (const json* old = m_Impl->Get(path)) {
            const json& oldComponents = (*old)["components"];
            for (auto [h, pc] : scene.Registry().view<PrefabComponent>().each()) {
                if (h == instance.GetHandle() || pc.Path != path) continue;
                if (ComponentsToJson(Entity(h, &scene), m_Impl->Resources) == oldComponents) followers.push_back(h);
            }
        }

        if (!Save(instance, path)) return -1;
        int updated = 0;
        for (auto h : followers)
            if (m_Impl->Fill(Entity(h, &scene), path)) updated++;
        ENGINE_INFO("Applied to prefab {}: {} other instance(s) updated", path, updated);
        return updated;
    }

    bool PrefabLibrary::Revert(Entity instance) {
        if (!instance.Has<PrefabComponent>()) return false;
        return m_Impl->Fill(instance, instance.Get<PrefabComponent>().Path);
    }

    bool PrefabLibrary::IsModified(Entity instance) {
        if (!instance.Has<PrefabComponent>()) return false;
        const json* p = m_Impl->Get(instance.Get<PrefabComponent>().Path);
        if (!p) return true;
        return ComponentsToJson(instance, m_Impl->Resources) != (*p)["components"];
    }

} // namespace Engine
