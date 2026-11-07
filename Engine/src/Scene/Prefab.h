#pragma once
#include "Scene/Entity.h"

#include <nlohmann/json_fwd.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Scene;
    struct ResourceRegistry;

    // Prefabs: an entity's components saved to their own file (Assets/Prefabs/<name>.json), placed
    // any number of times. An instance carries a PrefabComponent with the file's path.
    //
    // An instance is the prefab plus its OVERRIDES: the fields it changed, components it added,
    // components it removed. Scene files store only those, so a prefab edit reaches every field an
    // instance didn't override, in every level, the next time it loads (or right away on Apply).
    //   overrides = { "meshRenderer": { "color": [1,0,0] },   changed fields
    //                 "audio": { ...whole component... },     a component the prefab doesn't have
    //                 "spin": null }                          a prefab component removed
    //
    // A prefab file is { "version", "name", "transform", "components" }. Name and transform are
    // the defaults for new instances (rotation and scale; the position is the caller's).
    // Files are cached, re-read when they change on disk, and normalised (see Prefab.cpp).
    class PrefabLibrary {
    public:
        PrefabLibrary(std::string assetDir, const ResourceRegistry& resources);
        ~PrefabLibrary();
        PrefabLibrary(const PrefabLibrary&) = delete;
        PrefabLibrary& operator=(const PrefabLibrary&) = delete;

        // "coin" -> "Prefabs/coin.json"; paths with a folder or ".json" are kept as they are
        static std::string Resolve(const std::string& nameOrPath);

        bool Exists(const std::string& path) const;
        std::vector<std::string> List() const;          // every prefab in Assets/Prefabs, as paths

        // A new linked instance (prefab name, default rotation and scale, at the origin).
        // Returns an invalid Entity if the prefab can't be read.
        Entity Instantiate(Scene& scene, const std::string& path);

        // Make e an instance: the prefab's components with `overrides` on top (Tag, Transform,
        // ID and Parent kept)
        bool ApplyPrefabTo(Entity e, const std::string& path, const nlohmann::json& overrides);

        // Write e as a prefab file and link e to it. Overwrites an existing file.
        bool Save(Entity e, const std::string& path);

        // Write an instance (overrides and all) to its prefab. Every other instance in the scene
        // keeps its own overrides on top of the new prefab. Returns how many were updated (-1 on failure).
        int Apply(Scene& scene, Entity instance);

        bool Revert(Entity instance);           // drop every override
        bool IsModified(Entity instance);       // has overrides (or its prefab file is missing)
        bool CanLoad(const std::string& path); // the prefab file exists and parses

        // Overrides of a live instance ({} if none), and the same as a list of readable items:
        // "meshRenderer.color" (changed field), "+audio" (added), "-spin" (removed)
        nlohmann::json GetOverrides(Entity instance);
        std::vector<std::string> ListOverrides(Entity instance);
        bool RevertOverride(Entity instance, const std::string& item);   // one item from ListOverrides

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

    // The override format above: prefab components + overrides <-> instance components
    nlohmann::json DiffComponents(const nlohmann::json& prefab, const nlohmann::json& instance);
    nlohmann::json MergeOverrides(const nlohmann::json& prefab, const nlohmann::json& overrides);

} // namespace Engine
