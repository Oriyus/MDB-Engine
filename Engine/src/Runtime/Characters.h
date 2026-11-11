#pragma once
#include "Runtime/GameDatabase.h"
#include "Scene/Components.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <random>
#include <string>
#include <vector>

namespace Engine {

    class Scene;
    class PrefabLibrary;
    struct ResourceRegistry;

    // The rules for NPCs of the game database (NpcComponent) and items lying in the world (PickupComponent).
    // The script engine drives them (Lua `e.npc`, `db`, `scene.spawn_npc`...): it prepares new NPCs and ticks
    // their effects every frame, and turns deaths into drops and the "npc_died" event. The editor uses the
    // entity helpers (MakeNpc, MakePickup) to place them.
    //
    // A stat's value = its base (NpcComponent::Stats) + running modifiers + Modifier effects of equipped items,
    // clamped to the stat's Min..Max (Max: the MaxStat's value if it has one). Bases are kept in range too.
    namespace Characters {

        // Bring an NPC to life once: base stats (the stats' defaults, then the NPC's own values) and its
        // inventory (each entry kept by its chance; "equipped" ones start equipped, without firing Equip Add or
        // Event effects). No-op if Ready.
        void Prepare(NpcComponent& npc, const GameDatabase& db, std::mt19937& rng);

        float Value(const NpcComponent& npc, const GameDatabase& db, const std::string& stat);   // effective
        float Base(const NpcComponent& npc, const GameDatabase& db, const std::string& stat);
        // Set or change a base, clamped. True if this killed it (the death stat reached its minimum).
        bool SetBase(NpcComponent& npc, const GameDatabase& db, const std::string& stat, float value);
        bool AddBase(NpcComponent& npc, const GameDatabase& db, const std::string& stat, float delta);
        bool IsAtDeath(const NpcComponent& npc, const GameDatabase& db);

        // Effects advance: timed modifiers run out, OverTime effects add their share. True if it died.
        bool Tick(NpcComponent& npc, const GameDatabase& db, float dt);

        // ---- inventory
        int Count(const NpcComponent& npc, const std::string& item);
        // Add n (onto stacks of a stackable item, up to MaxStack each, else new entries). The number added.
        int Give(NpcComponent& npc, const GameDatabase& db, const std::string& item, int n = 1, bool drop = true);
        // Remove n (unequipped stacks first). False and nothing removed if there aren't that many.
        bool Take(NpcComponent& npc, const std::string& item, int n = 1);

        // What applying an item did besides changing stats: events to send ({ name, amount }) and a death
        struct Outcome {
            bool Died = false;
            std::vector<std::pair<std::string, float>> Events;
        };
        // Apply the item's effects of one trigger to this NPC (Hit: on what a weapon hits)
        Outcome Apply(NpcComponent& npc, const GameDatabase& db, const std::string& item, EffectTrigger trigger);
        // Use an item it carries: its Use effects, and one is used up if it is consumable. False (error says why)
        // if it has none or nothing happens on use.
        bool Use(NpcComponent& npc, const GameDatabase& db, const std::string& item, Outcome& out, std::string* error = nullptr);
        // Equip (or unequip) one it carries in the item's slot; whatever was in that slot comes off.
        bool Equip(NpcComponent& npc, const GameDatabase& db, const std::string& item, bool on, Outcome& out, std::string* error = nullptr);

        // Mark it dead and take out what it drops (entries with Drop); the rest stays in its inventory
        std::vector<NpcItem> Die(NpcComponent& npc);

        // ---- entities
        // A model ("model:<path>" geometry scaled to `height`, an Animator if it is skinned) or a prefab instance.
        // Invalid (null) if it can't be loaded or path is empty.
        entt::entity MakeVisual(Scene& scene, const std::string& path, float height, const ResourceRegistry& res,
                                PrefabLibrary* prefabs, const std::string& name);
        // A new NPC entity at the origin: its model (or an empty entity), NpcComponent, the animation graph, script
        // and capsule body of its definition. Unknown id: an empty entity with just the NpcComponent.
        entt::entity MakeNpc(Scene& scene, const GameDatabase& db, const std::string& id, const ResourceRegistry& res,
                             PrefabLibrary* prefabs);
        // Make an existing NPC entity look as its definition says again (model, height, graph, script, body).
        // A prefab model is left alone (the entity already is what it is).
        void ApplyLook(Scene& scene, entt::entity e, const NpcDef& def, const ResourceRegistry& res);
        // An item on the ground at a world position: its model (or a small box), PickupComponent, a slow spin
        entt::entity MakePickup(Scene& scene, const GameDatabase& db, const std::string& item, int count,
                                const glm::vec3& position, const ResourceRegistry& res, PrefabLibrary* prefabs);
    }

} // namespace Engine
