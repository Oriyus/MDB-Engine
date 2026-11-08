#pragma once
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Engine {

    class Scene;
    class PrefabLibrary;
    struct ResourceRegistry;

    // A level as a save game stores it: what differs from its scene file. Entities are matched by
    // their IDComponent; anything changed or added is stored whole, removed ones by ID.
    //   { "entities": [ entity state... ], "removed": [ id... ] }
    // Transient entities (TransientComponent) and everything under them are never stored.

    // One entity: name, id, parent, transform, prefab link and every component (prefab parts are
    // stored in full, so a changed prefab file doesn't change what a save restores)
    nlohmann::json EntityState(Scene& scene, entt::entity e, const ResourceRegistry& resources);
    // Create the entity, or update it in place if one with that id exists. Throws on malformed values.
    entt::entity ApplyEntityState(Scene& scene, const nlohmann::json& j, const ResourceRegistry& resources);

    // A scene file's entities as they load: id -> compact EntityState text
    using LevelBase = std::unordered_map<uint64_t, std::string>;
    // False if the file can't be read (the base is then empty: every entity counts as added)
    bool ReadLevelBase(const std::string& path, const ResourceRegistry& resources, PrefabLibrary* prefabs, LevelBase& out);

    // The live scene against its base. Entities in `exclude` (by id) and their subtrees count as absent.
    nlohmann::json DiffLevel(Scene& scene, const LevelBase& base, const ResourceRegistry& resources,
        const std::unordered_set<uint64_t>& exclude = {});
    // Apply a DiffLevel result to a scene freshly loaded from the same file
    void ApplyLevelDiff(Scene& scene, const nlohmann::json& diff, const ResourceRegistry& resources);

    // e and every entity below it, parents before children
    std::vector<entt::entity> Subtree(Scene& scene, entt::entity e);

} // namespace Engine
