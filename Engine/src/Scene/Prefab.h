#pragma once
#include "Scene/Entity.h"

#include <nlohmann/json_fwd.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Scene;
    struct ResourceRegistry;

    // Prefabs: an entity with its children, saved to their own file (Assets/Prefabs/<name>.json)
    // and placed any number of times.
    //
    // A prefab is a tree of NODES. The root node has the components and default rotation/scale;
    // each child node has a stable "key", a name, a local transform, components and children:
    //   { "version": 2, "name", "transform", "components",
    //     "children": [ { "key", "name", "transform", "components", "children": [...] } ] }
    // (Version 1 files are the same without children.)
    //
    // Every entity of an instance links to its node with PrefabComponent{ Path, Node } (Node 0 =
    // the root). Each one is its node plus OVERRIDES: the fields it changed, components it added,
    // components it removed, and for child nodes a different local transform:
    //   overrides = { "meshRenderer": { "color": [1,0,0] },   changed fields
    //                 "audio": { ...whole component... },     a component the node doesn't have
    //                 "spin": null }                          a node component removed
    // Scene files store only links and overrides (plus a child's transform if it moved), so a
    // prefab edit reaches every instance, in every level, the next time it loads or right away on
    // Apply. An instance also remembers which child nodes were deleted from it ("removedNodes").
    //
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
        bool CanLoad(const std::string& path);          // the file exists and parses
        bool HasNode(const std::string& path, uint64_t node);

        // A new instance with all its children, the root at the origin with the prefab's default
        // rotation and scale. Returns an invalid Entity if the prefab can't be read.
        Entity Instantiate(Scene& scene, const std::string& path);

        // Make e the given node of a prefab: its components with `overrides` on top. Tag, ID and
        // Parent are kept; the transform too unless takeNodeTransform (child nodes only).
        bool ApplyPrefabTo(Entity e, const std::string& path, uint64_t node, const nlohmann::json& overrides,
            bool takeNodeTransform = false);

        // After loading: give an instance the child nodes the prefab has and it lacks, except the
        // ones in `skip` (deleted from this instance on purpose). Unlinks parts the prefab dropped.
        void SyncInstance(Scene& scene, Entity root, const std::vector<uint64_t>& skip);
        // Child nodes of the prefab that this instance doesn't have (it deleted them)
        std::vector<uint64_t> MissingNodes(Scene& scene, Entity root);

        // Write e and all its children as a prefab file, and link them to it. Overwrites the file.
        // Children that belonged to another prefab become parts of this one (prefabs don't nest).
        bool Save(Entity e, const std::string& path);

        // Write the instance that `part` belongs to (any part works) to its prefab. Every other
        // instance in the scene keeps its own overrides on top of the new prefab, gains parts the
        // prefab gained, and keeps (unlinked) parts the prefab lost. Returns how many other
        // instances were updated (-1 on failure).
        int Apply(Scene& scene, Entity part);

        Entity InstanceRoot(Entity part);       // the root of the instance `part` belongs to

        bool Revert(Entity part);               // drop every override of this one entity
        bool IsModified(Entity part);           // has overrides (or its prefab can't be read)
        bool TransformOverridden(Entity part);  // a child part moved away from its node's transform

        // Overrides of a live part ({} if none), and as readable items: "meshRenderer.color"
        // (changed field), "+audio" (added), "-spin" (removed), "transform" (a moved child part)
        nlohmann::json GetOverrides(Entity part);
        std::vector<std::string> ListOverrides(Entity part);
        bool RevertOverride(Entity part, const std::string& item);   // one item from ListOverrides

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

    // The override format above: node components + overrides <-> entity components
    nlohmann::json DiffComponents(const nlohmann::json& prefab, const nlohmann::json& instance);
    nlohmann::json MergeOverrides(const nlohmann::json& prefab, const nlohmann::json& overrides);

} // namespace Engine
