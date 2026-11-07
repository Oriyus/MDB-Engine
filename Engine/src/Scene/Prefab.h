#pragma once
#include "Scene/Entity.h"

#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Scene;
    struct ResourceRegistry;

    // Prefabs: an entity's components saved to their own file (Assets/Prefabs/<name>.json), placed
    // any number of times. An instance carries a PrefabComponent with the file's path.
    //
    // An instance that matches its prefab is saved in the scene as just name + transform + link,
    // and picks up the prefab's current components whenever the scene loads, so editing a prefab
    // updates every level that uses it. An instance whose components were changed is "modified": it
    // is saved with its own full copy (still linked) until it is applied to the prefab or reverted.
    //
    // A prefab file is { "version", "name", "transform", "components" }. Name and transform are
    // the defaults for new instances (rotation and scale; the position is the caller's).
    // Files are cached and re-read when they change on disk.
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

        // Replace e's components with the prefab's (Tag, Transform kept) and link e to it
        bool ApplyPrefabTo(Entity e, const std::string& path);

        // Write e as a prefab file and link e to it. Overwrites an existing file.
        bool Save(Entity e, const std::string& path);

        // Write an instance back to its prefab, then update every other instance in the scene that
        // still matched the old version. Returns how many instances were updated (-1 on failure).
        int Apply(Scene& scene, Entity instance);

        bool Revert(Entity instance);           // back to the prefab's components
        bool IsModified(Entity instance);       // components differ from the prefab's (or no file)

    private:
        struct Impl;                            // keeps nlohmann/json out of this header
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine
