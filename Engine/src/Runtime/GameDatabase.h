#pragma once
#include <nlohmann/json_fwd.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    // The game's design data: stat definitions, items and NPCs, edited in the editor's Database window and
    // stored as one file, Assets/Data/database.json (GameDatabase::kPath). Read-only at run time: what changes
    // while playing (an NPC's health, its inventory) lives in its NpcComponent, so save games keep it.
    //
    //   { "version": 1, "deathStat": "health",
    //     "stats": [ { "id", "name", "default", "min", "max", "maxStat", "description" } ],
    //     "items": [ { "id", "name", "description", "kind", "slot", "icon", "model", "modelHeight", "value",
    //                  "stackable", "maxStack", "consumable", "stats": { "<name>": n }, "effects": [ effect ] } ],
    //     "npcs":  [ { "id", "name", "description", "model", "height", "animGraph", "script", "dialogue",
    //                  "collider", "radius", "removeOnDeath", "deathEvent", "stats": { "<stat id>": n },
    //                  "inventory": [ { "item", "count", "chance", "drop", "equipped" } ] } ] }
    //   effect: { "trigger": "use"|"equip"|"hit", "kind": "add"|"modifier"|"overTime"|"event",
    //             "stat", "amount", "duration", "event" }

    // A stat every NPC has (health, attack...). Values are clamped to Min..Max; MaxStat names another stat whose
    // current value is the ceiling instead (health up to max_health).
    struct StatDef {
        std::string Id, Name, Description;
        float Default = 0.0f;
        float Min = 0.0f, Max = 100.0f;
        std::string MaxStat;
    };

    // When an item's effect happens: Use (npc:use, consumes a consumable), Equip (while equipped; Add and
    // Event happen once when equipping), Hit (npc:apply(item, "hit"): a weapon's effect on what it hits).
    enum class EffectTrigger { Use, Equip, Hit };
    // Add: Amount to the stat at once (a heal, damage). Modifier: +Amount to the stat for Duration seconds
    // (0 = for good; Equip: while equipped). OverTime: Amount per second for Duration seconds (poison,
    // regeneration). Event: events.emit(Event, { entity, item, amount }) for scripts to handle.
    enum class EffectKind { Add, Modifier, OverTime, Event };
    struct ItemEffect {
        EffectTrigger Trigger = EffectTrigger::Use;
        EffectKind Kind = EffectKind::Add;
        std::string Stat;
        float Amount = 0.0f;
        float Duration = 0.0f;
        std::string Event;
    };

    struct ItemDef {
        std::string Id, Name, Description;    // description: UI markup ({#rrggbb}..{/}, {b}..{/b})
        std::string Kind = "misc";            // weapon, armour, consumable, quest, treasure, misc (free text)
        std::string Slot;                     // where it is equipped (head, weapon...); "" = can't be equipped
        std::string Icon;                     // image under Assets ("Textures/Icons/sword.png")
        std::string Model;                    // model or prefab under Assets: how it looks lying on the ground
        float ModelHeight = 0.4f;             // metres (a model is scaled to it)
        int Value = 0;
        bool Stackable = false;
        int MaxStack = 99;
        bool Consumable = false;              // used up by npc:use
        std::map<std::string, float> Stats;   // its own numbers for scripts: damage_min, armour...
        std::vector<ItemEffect> Effects;
    };

    // One line of an NPC's starting inventory. Chance (0..1) is rolled once when the NPC first comes to life.
    struct InventoryEntry {
        std::string Item;
        int Count = 1;
        float Chance = 1.0f;
        bool Drop = true;                     // dropped when it dies
        bool Equipped = false;
    };

    struct NpcDef {
        std::string Id, Name, Description;
        std::string Model;                    // model or prefab under Assets ("" = whatever the entity has)
        float Height = 1.8f;                  // metres (a model is scaled to it)
        std::string AnimGraph, Script, Dialogue;   // "AnimGraphs/x.json", "Scripts/x.lua", "Dialogues/x.json"
        bool Collider = true;                 // a kinematic capsule body (clicks and raycasts hit it)
        float Radius = 0.4f;
        bool RemoveOnDeath = true;            // the entity is removed when it dies (after its loot drops)
        std::string DeathEvent;               // also sent when it dies (besides "npc_died")
        std::map<std::string, float> Stats;   // overrides of the stats' defaults
        std::vector<InventoryEntry> Inventory;
    };

    struct GameDatabase {
        static constexpr const char* kPath = "Data/database.json";   // under Assets

        std::string DeathStat = "health";     // an NPC dies when this reaches its minimum
        std::vector<StatDef> Stats;
        std::vector<ItemDef> Items;
        std::vector<NpcDef> Npcs;

        const StatDef* Stat(const std::string& id) const;
        const ItemDef* Item(const std::string& id) const;
        const NpcDef* Npc(const std::string& id) const;

        nlohmann::json ToJson() const;
        // Missing fields take their defaults. False (with a message) if it isn't a database at all.
        static bool FromJson(const nlohmann::json& j, GameDatabase& out, std::string* error = nullptr);
        static std::shared_ptr<GameDatabase> Load(const std::string& file, std::string* error = nullptr);
        bool Save(const std::string& file) const;    // written to a temporary file, then renamed

        // Problems worth fixing: empty or repeated ids, unknown items and stats, missing files (assetDir: to check them)
        std::vector<std::string> Issues(const std::string& assetDir = "") const;
    };

    const char* EffectTriggerName(EffectTrigger t);
    const char* EffectKindName(EffectKind k);

} // namespace Engine
