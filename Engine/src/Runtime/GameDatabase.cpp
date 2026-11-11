#include "Runtime/GameDatabase.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

namespace Engine {

    namespace {
        using json = nlohmann::json;

        template<typename T>
        T Get(const json& j, const char* key, T def) {
            auto it = j.find(key);
            if (it == j.end() || it->is_null()) return def;
            try { return it->get<T>(); }
            catch (...) { return def; }
        }

        EffectTrigger TriggerFrom(const std::string& s) {
            if (s == "equip") return EffectTrigger::Equip;
            if (s == "hit") return EffectTrigger::Hit;
            return EffectTrigger::Use;
        }
        EffectKind KindFrom(const std::string& s) {
            if (s == "modifier") return EffectKind::Modifier;
            if (s == "overTime") return EffectKind::OverTime;
            if (s == "event") return EffectKind::Event;
            return EffectKind::Add;
        }

        json StatsJson(const std::map<std::string, float>& m) {
            json j = json::object();
            for (const auto& [k, v] : m) j[k] = v;
            return j;
        }
        std::map<std::string, float> StatsFrom(const json& j, const char* key) {
            std::map<std::string, float> m;
            auto it = j.find(key);
            if (it == j.end() || !it->is_object()) return m;
            for (auto e = it->begin(); e != it->end(); ++e)
                if (e->is_number()) m[e.key()] = e->get<float>();
            return m;
        }
        // Optional strings are written only when set, so the file stays short
        void Put(json& j, const char* key, const std::string& s) { if (!s.empty()) j[key] = s; }
    }

    const char* EffectTriggerName(EffectTrigger t) {
        return t == EffectTrigger::Equip ? "equip" : t == EffectTrigger::Hit ? "hit" : "use";
    }
    const char* EffectKindName(EffectKind k) {
        return k == EffectKind::Modifier ? "modifier" : k == EffectKind::OverTime ? "overTime" : k == EffectKind::Event ? "event" : "add";
    }

    const StatDef* GameDatabase::Stat(const std::string& id) const {
        for (const auto& s : Stats) if (s.Id == id) return &s;
        return nullptr;
    }
    const ItemDef* GameDatabase::Item(const std::string& id) const {
        for (const auto& s : Items) if (s.Id == id) return &s;
        return nullptr;
    }
    const NpcDef* GameDatabase::Npc(const std::string& id) const {
        for (const auto& s : Npcs) if (s.Id == id) return &s;
        return nullptr;
    }

    json GameDatabase::ToJson() const {
        json j;
        j["version"] = 1;
        j["deathStat"] = DeathStat;
        json stats = json::array();
        for (const auto& s : Stats) {
            json o{ { "id", s.Id }, { "name", s.Name }, { "default", s.Default }, { "min", s.Min }, { "max", s.Max } };
            Put(o, "maxStat", s.MaxStat);
            Put(o, "description", s.Description);
            stats.push_back(o);
        }
        j["stats"] = stats;
        json items = json::array();
        for (const auto& it : Items) {
            json o{ { "id", it.Id }, { "name", it.Name }, { "kind", it.Kind }, { "value", it.Value } };
            Put(o, "description", it.Description);
            Put(o, "slot", it.Slot);
            Put(o, "icon", it.Icon);
            Put(o, "model", it.Model);
            if (!it.Model.empty()) o["modelHeight"] = it.ModelHeight;
            if (it.Stackable) { o["stackable"] = true; o["maxStack"] = it.MaxStack; }
            if (it.Consumable) o["consumable"] = true;
            if (!it.Stats.empty()) o["stats"] = StatsJson(it.Stats);
            if (!it.Effects.empty()) {
                json fx = json::array();
                for (const auto& e : it.Effects) {
                    json f{ { "trigger", EffectTriggerName(e.Trigger) }, { "kind", EffectKindName(e.Kind) } };
                    if (e.Kind == EffectKind::Event) Put(f, "event", e.Event);
                    else Put(f, "stat", e.Stat);
                    f["amount"] = e.Amount;
                    if (e.Kind == EffectKind::Modifier || e.Kind == EffectKind::OverTime) f["duration"] = e.Duration;
                    fx.push_back(f);
                }
                o["effects"] = fx;
            }
            items.push_back(o);
        }
        j["items"] = items;
        json npcs = json::array();
        for (const auto& n : Npcs) {
            json o{ { "id", n.Id }, { "name", n.Name }, { "height", n.Height }, { "collider", n.Collider },
                    { "removeOnDeath", n.RemoveOnDeath } };
            Put(o, "description", n.Description);
            Put(o, "model", n.Model);
            Put(o, "animGraph", n.AnimGraph);
            Put(o, "script", n.Script);
            Put(o, "dialogue", n.Dialogue);
            Put(o, "deathEvent", n.DeathEvent);
            if (n.Collider) o["radius"] = n.Radius;
            if (!n.Stats.empty()) o["stats"] = StatsJson(n.Stats);
            json inv = json::array();
            for (const auto& e : n.Inventory) {
                json r{ { "item", e.Item }, { "count", e.Count } };
                if (e.Chance < 1.0f) r["chance"] = e.Chance;
                if (!e.Drop) r["drop"] = false;
                if (e.Equipped) r["equipped"] = true;
                inv.push_back(r);
            }
            if (!inv.empty()) o["inventory"] = inv;
            npcs.push_back(o);
        }
        j["npcs"] = npcs;
        return j;
    }

    bool GameDatabase::FromJson(const json& j, GameDatabase& out, std::string* error) {
        if (!j.is_object()) { if (error) *error = "not a JSON object"; return false; }
        GameDatabase db;
        db.DeathStat = Get<std::string>(j, "deathStat", db.DeathStat);
        if (auto it = j.find("stats"); it != j.end() && it->is_array())
            for (const auto& o : *it) {
                if (!o.is_object()) continue;
                StatDef s;
                s.Id = Get<std::string>(o, "id", "");
                s.Name = Get<std::string>(o, "name", s.Id);
                s.Description = Get<std::string>(o, "description", "");
                s.Default = Get<float>(o, "default", s.Default);
                s.Min = Get<float>(o, "min", s.Min);
                s.Max = Get<float>(o, "max", s.Max);
                s.MaxStat = Get<std::string>(o, "maxStat", "");
                db.Stats.push_back(s);
            }
        if (auto it = j.find("items"); it != j.end() && it->is_array())
            for (const auto& o : *it) {
                if (!o.is_object()) continue;
                ItemDef d;
                d.Id = Get<std::string>(o, "id", "");
                d.Name = Get<std::string>(o, "name", d.Id);
                d.Description = Get<std::string>(o, "description", "");
                d.Kind = Get<std::string>(o, "kind", d.Kind);
                d.Slot = Get<std::string>(o, "slot", "");
                d.Icon = Get<std::string>(o, "icon", "");
                d.Model = Get<std::string>(o, "model", "");
                d.ModelHeight = Get<float>(o, "modelHeight", d.ModelHeight);
                d.Value = Get<int>(o, "value", 0);
                d.Stackable = Get<bool>(o, "stackable", false);
                d.MaxStack = std::max(1, Get<int>(o, "maxStack", d.MaxStack));
                d.Consumable = Get<bool>(o, "consumable", false);
                d.Stats = StatsFrom(o, "stats");
                if (auto fx = o.find("effects"); fx != o.end() && fx->is_array())
                    for (const auto& f : *fx) {
                        if (!f.is_object()) continue;
                        ItemEffect e;
                        e.Trigger = TriggerFrom(Get<std::string>(f, "trigger", "use"));
                        e.Kind = KindFrom(Get<std::string>(f, "kind", "add"));
                        e.Stat = Get<std::string>(f, "stat", "");
                        e.Amount = Get<float>(f, "amount", 0.0f);
                        e.Duration = Get<float>(f, "duration", 0.0f);
                        e.Event = Get<std::string>(f, "event", "");
                        d.Effects.push_back(e);
                    }
                db.Items.push_back(d);
            }
        if (auto it = j.find("npcs"); it != j.end() && it->is_array())
            for (const auto& o : *it) {
                if (!o.is_object()) continue;
                NpcDef n;
                n.Id = Get<std::string>(o, "id", "");
                n.Name = Get<std::string>(o, "name", n.Id);
                n.Description = Get<std::string>(o, "description", "");
                n.Model = Get<std::string>(o, "model", "");
                n.Height = Get<float>(o, "height", n.Height);
                n.AnimGraph = Get<std::string>(o, "animGraph", "");
                n.Script = Get<std::string>(o, "script", "");
                n.Dialogue = Get<std::string>(o, "dialogue", "");
                n.Collider = Get<bool>(o, "collider", n.Collider);
                n.Radius = Get<float>(o, "radius", n.Radius);
                n.RemoveOnDeath = Get<bool>(o, "removeOnDeath", n.RemoveOnDeath);
                n.DeathEvent = Get<std::string>(o, "deathEvent", "");
                n.Stats = StatsFrom(o, "stats");
                if (auto inv = o.find("inventory"); inv != o.end() && inv->is_array())
                    for (const auto& r : *inv) {
                        if (!r.is_object()) continue;
                        InventoryEntry e;
                        e.Item = Get<std::string>(r, "item", "");
                        e.Count = std::max(1, Get<int>(r, "count", 1));
                        e.Chance = std::clamp(Get<float>(r, "chance", 1.0f), 0.0f, 1.0f);
                        e.Drop = Get<bool>(r, "drop", true);
                        e.Equipped = Get<bool>(r, "equipped", false);
                        n.Inventory.push_back(e);
                    }
                db.Npcs.push_back(n);
            }
        out = std::move(db);
        return true;
    }

    std::shared_ptr<GameDatabase> GameDatabase::Load(const std::string& file, std::string* error) {
        std::ifstream in(std::filesystem::u8path(file));
        if (!in) { if (error) *error = "can't open the file"; return nullptr; }
        try {
            json j;
            in >> j;
            auto db = std::make_shared<GameDatabase>();
            if (!FromJson(j, *db, error)) return nullptr;
            return db;
        }
        catch (const std::exception& ex) {
            if (error) *error = ex.what();
            return nullptr;
        }
    }

    bool GameDatabase::Save(const std::string& file) const {
        namespace fs = std::filesystem;
        const fs::path path = fs::u8path(file), tmp = fs::u8path(file + ".tmp");
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        {
            std::ofstream out(tmp, std::ios::binary);
            if (!out) return false;
            out << ToJson().dump(2) << "\n";
            if (!out) return false;
        }
        fs::rename(tmp, path, ec);
        return !ec;
    }

    std::vector<std::string> GameDatabase::Issues(const std::string& assetDir) const {
        std::vector<std::string> out;
        auto missing = [&](const std::string& path) {
            if (path.empty() || assetDir.empty()) return false;
            std::error_code ec;
            return !std::filesystem::exists(std::filesystem::u8path(assetDir + path), ec);
        };
        auto ids = [&](const char* what, const auto& list) {
            std::set<std::string> seen;
            for (const auto& e : list) {
                if (e.Id.empty()) out.push_back(std::string("A ") + what + " has no id");
                else if (!seen.insert(e.Id).second) out.push_back(std::string("Two ") + what + "s have the id \"" + e.Id + "\"");
            }
        };
        ids("stat", Stats);
        ids("item", Items);
        ids("NPC", Npcs);
        if (!DeathStat.empty() && !Stat(DeathStat)) out.push_back("The death stat \"" + DeathStat + "\" is not a stat");
        for (const auto& s : Stats)
            if (!s.MaxStat.empty() && !Stat(s.MaxStat)) out.push_back("Stat " + s.Id + ": its maximum \"" + s.MaxStat + "\" is not a stat");
        for (const auto& it : Items) {
            const std::string who = "Item " + (it.Id.empty() ? it.Name : it.Id);
            if (missing(it.Icon)) out.push_back(who + ": no icon file " + it.Icon);
            if (missing(it.Model)) out.push_back(who + ": no model file " + it.Model);
            for (const auto& e : it.Effects) {
                if (e.Kind == EffectKind::Event) { if (e.Event.empty()) out.push_back(who + ": an event effect without an event name"); }
                else if (!Stat(e.Stat)) out.push_back(who + ": an effect on \"" + e.Stat + "\", which is not a stat");
                if (e.Trigger == EffectTrigger::Equip && it.Slot.empty()) out.push_back(who + ": an equip effect, but no slot to equip it in");
            }
            if (it.Consumable && std::none_of(it.Effects.begin(), it.Effects.end(), [](const ItemEffect& e) { return e.Trigger == EffectTrigger::Use; }))
                out.push_back(who + ": consumable, but nothing happens on use");
        }
        for (const auto& n : Npcs) {
            const std::string who = "NPC " + (n.Id.empty() ? n.Name : n.Id);
            if (missing(n.Model)) out.push_back(who + ": no model file " + n.Model);
            if (missing(n.AnimGraph)) out.push_back(who + ": no animation graph " + n.AnimGraph);
            if (missing(n.Script)) out.push_back(who + ": no script " + n.Script);
            if (missing(n.Dialogue)) out.push_back(who + ": no dialogue " + n.Dialogue);
            for (const auto& [k, v] : n.Stats) if (!Stat(k)) out.push_back(who + ": a value for \"" + k + "\", which is not a stat");
            std::set<std::string> slots;
            for (const auto& e : n.Inventory) {
                const ItemDef* item = Item(e.Item);
                if (!item) { out.push_back(who + ": its inventory has \"" + e.Item + "\", which is not an item"); continue; }
                if (e.Equipped) {
                    if (item->Slot.empty()) out.push_back(who + ": " + item->Name + " is equipped but has no slot");
                    else if (!slots.insert(item->Slot).second) out.push_back(who + ": two items equipped in the " + item->Slot + " slot");
                }
            }
        }
        return out;
    }

} // namespace Engine
