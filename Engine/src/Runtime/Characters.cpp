#include "Runtime/Characters.h"

#include "Scene/Scene.h"
#include "Scene/Entity.h"
#include "Scene/Prefab.h"
#include "Scene/SceneSerializer.h"
#include "Renderer/Model.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace Engine::Characters {

    namespace {
        float RawBase(const NpcComponent& npc, const GameDatabase& db, const std::string& stat) {
            if (auto it = npc.Stats.find(stat); it != npc.Stats.end()) return it->second;
            const StatDef* d = db.Stat(stat);
            return d ? d->Default : 0.0f;
        }

        // Everything on top of the base: running modifiers and the Modifier effects of equipped items
        float Bonus(const NpcComponent& npc, const GameDatabase& db, const std::string& stat) {
            float b = 0.0f;
            for (const auto& e : npc.Effects) if (!e.OverTime && e.Stat == stat) b += e.Amount;
            for (const auto& it : npc.Inventory) {
                if (!it.Equipped) continue;
                const ItemDef* item = db.Item(it.Item);
                if (!item) continue;
                for (const auto& fx : item->Effects)
                    if (fx.Trigger == EffectTrigger::Equip && fx.Kind == EffectKind::Modifier && fx.Stat == stat) b += fx.Amount;
            }
            return b;
        }

        // The stat's range now (a MaxStat's value is the ceiling; depth guards a loop of MaxStats)
        void Range(const NpcComponent& npc, const GameDatabase& db, const StatDef& d, float& lo, float& hi, int depth) {
            lo = d.Min;
            hi = d.Max;
            if (!d.MaxStat.empty() && d.MaxStat != d.Id && depth < 4)
                if (const StatDef* m = db.Stat(d.MaxStat)) {
                    float mlo, mhi;
                    Range(npc, db, *m, mlo, mhi, depth + 1);
                    hi = std::clamp(RawBase(npc, db, m->Id) + Bonus(npc, db, m->Id), mlo, mhi);
                }
            if (hi < lo) hi = lo;
        }

        float ValueAt(const NpcComponent& npc, const GameDatabase& db, const std::string& stat, int depth) {
            const float v = RawBase(npc, db, stat) + Bonus(npc, db, stat);
            const StatDef* d = db.Stat(stat);
            if (!d) return v;
            float lo, hi;
            Range(npc, db, *d, lo, hi, depth);
            return std::clamp(v, lo, hi);
        }

        // Keep every base in its range (after a ceiling stat went down)
        void ClampAll(NpcComponent& npc, const GameDatabase& db) {
            for (auto& [id, v] : npc.Stats)
                if (const StatDef* d = db.Stat(id)) {
                    float lo, hi;
                    Range(npc, db, *d, lo, hi, 0);
                    v = std::clamp(v, lo, hi);
                }
        }

        bool DiedNow(NpcComponent& npc, const GameDatabase& db) { return !npc.Dead && IsAtDeath(npc, db); }
    }

    void Prepare(NpcComponent& npc, const GameDatabase& db, std::mt19937& rng) {
        if (npc.Ready) return;
        npc.Ready = true;
        npc.Dead = false;
        npc.Stats.clear();
        npc.Inventory.clear();
        npc.Effects.clear();
        const NpcDef* def = db.Npc(npc.Id);
        for (const auto& s : db.Stats) npc.Stats[s.Id] = s.Default;
        if (def) {
            for (const auto& [k, v] : def->Stats) npc.Stats[k] = v;
            std::uniform_real_distribution<float> U(0.0f, 1.0f);
            for (const auto& e : def->Inventory) {
                if (e.Chance < 1.0f && U(rng) >= e.Chance) continue;
                const size_t before = npc.Inventory.size();
                Give(npc, db, e.Item, e.Count, e.Drop);
                if (e.Equipped && npc.Inventory.size() > before) npc.Inventory[before].Equipped = true;
            }
        }
        ClampAll(npc, db);
    }

    float Value(const NpcComponent& npc, const GameDatabase& db, const std::string& stat) { return ValueAt(npc, db, stat, 0); }
    float Base(const NpcComponent& npc, const GameDatabase& db, const std::string& stat) { return RawBase(npc, db, stat); }

    bool IsAtDeath(const NpcComponent& npc, const GameDatabase& db) {
        const StatDef* d = db.Stat(db.DeathStat);
        return d && Value(npc, db, d->Id) <= d->Min;
    }

    bool SetBase(NpcComponent& npc, const GameDatabase& db, const std::string& stat, float value) {
        npc.Stats[stat] = value;
        ClampAll(npc, db);
        return DiedNow(npc, db);
    }

    bool AddBase(NpcComponent& npc, const GameDatabase& db, const std::string& stat, float delta) {
        return SetBase(npc, db, stat, RawBase(npc, db, stat) + delta);
    }

    bool Tick(NpcComponent& npc, const GameDatabase& db, float dt) {
        if (npc.Dead || npc.Effects.empty() || dt <= 0.0f) return false;
        bool changed = false;
        for (size_t i = 0; i < npc.Effects.size();) {
            NpcEffect& e = npc.Effects[i];
            const float step = e.Remaining < 0.0f ? dt : std::min(dt, e.Remaining);
            if (e.OverTime) npc.Stats[e.Stat] = RawBase(npc, db, e.Stat) + e.Amount * step;
            changed = true;
            if (e.Remaining >= 0.0f) {
                e.Remaining -= dt;
                if (e.Remaining <= 0.0f) { npc.Effects.erase(npc.Effects.begin() + i); continue; }
            }
            ++i;
        }
        if (changed) ClampAll(npc, db);
        return DiedNow(npc, db);
    }

    int Count(const NpcComponent& npc, const std::string& item) {
        int n = 0;
        for (const auto& it : npc.Inventory) if (it.Item == item) n += it.Count;
        return n;
    }

    int Give(NpcComponent& npc, const GameDatabase& db, const std::string& item, int n, bool drop) {
        const ItemDef* def = db.Item(item);
        if (!def || n <= 0) return 0;
        int left = n;
        if (def->Stackable) {
            for (auto& it : npc.Inventory) {
                if (it.Item != item || it.Equipped || left <= 0) continue;
                const int add = std::min(left, def->MaxStack - it.Count);
                if (add > 0) { it.Count += add; left -= add; }
            }
            while (left > 0) {
                const int add = std::min(left, def->MaxStack);
                npc.Inventory.push_back({ item, add, false, drop });
                left -= add;
            }
        }
        else
            for (; left > 0; left--) npc.Inventory.push_back({ item, 1, false, drop });
        return n;
    }

    bool Take(NpcComponent& npc, const std::string& item, int n) {
        if (n <= 0) return true;
        if (Count(npc, item) < n) return false;
        for (int pass = 0; pass < 2 && n > 0; pass++)          // unequipped stacks first
            for (auto it = npc.Inventory.begin(); it != npc.Inventory.end() && n > 0;) {
                if (it->Item != item || it->Equipped != (pass == 1)) { ++it; continue; }
                const int k = std::min(n, it->Count);
                it->Count -= k;
                n -= k;
                if (it->Count <= 0) it = npc.Inventory.erase(it); else ++it;
            }
        return true;
    }

    Outcome Apply(NpcComponent& npc, const GameDatabase& db, const std::string& item, EffectTrigger trigger) {
        Outcome out;
        const ItemDef* def = db.Item(item);
        if (!def || npc.Dead) return out;
        for (const auto& fx : def->Effects) {
            if (fx.Trigger != trigger) continue;
            switch (fx.Kind) {
            case EffectKind::Add:
                npc.Stats[fx.Stat] = RawBase(npc, db, fx.Stat) + fx.Amount;
                break;
            case EffectKind::Modifier:     // an equipped item's modifiers count while it is equipped (Bonus)
                if (trigger != EffectTrigger::Equip)
                    npc.Effects.push_back({ fx.Stat, item, fx.Amount, fx.Duration > 0.0f ? fx.Duration : -1.0f, false });
                break;
            case EffectKind::OverTime:
                npc.Effects.push_back({ fx.Stat, item, fx.Amount, fx.Duration > 0.0f ? fx.Duration : -1.0f, true });
                break;
            case EffectKind::Event:
                out.Events.push_back({ fx.Event, fx.Amount });
                break;
            }
        }
        ClampAll(npc, db);
        out.Died = DiedNow(npc, db);
        return out;
    }

    bool Use(NpcComponent& npc, const GameDatabase& db, const std::string& item, Outcome& out, std::string* error) {
        const ItemDef* def = db.Item(item);
        auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
        if (!def) return fail("no item \"" + item + "\" in the database");
        if (Count(npc, item) <= 0) return fail("doesn't carry " + def->Name);
        if (npc.Dead) return fail("is dead");
        if (std::none_of(def->Effects.begin(), def->Effects.end(), [](const ItemEffect& e) { return e.Trigger == EffectTrigger::Use; }))
            return fail(def->Name + " does nothing when used");
        out = Apply(npc, db, item, EffectTrigger::Use);
        if (def->Consumable) Take(npc, item, 1);
        return true;
    }

    bool Equip(NpcComponent& npc, const GameDatabase& db, const std::string& item, bool on, Outcome& out, std::string* error) {
        const ItemDef* def = db.Item(item);
        auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
        if (!def) return fail("no item \"" + item + "\" in the database");
        if (!on) {
            for (auto& it : npc.Inventory)
                if (it.Item == item && it.Equipped) { it.Equipped = false; ClampAll(npc, db); return true; }
            return fail(def->Name + " is not equipped");
        }
        if (def->Slot.empty()) return fail(def->Name + " can't be equipped (no slot)");
        NpcItem* target = nullptr;
        for (auto& it : npc.Inventory) if (it.Item == item && !it.Equipped) { target = &it; break; }
        if (!target) return fail("doesn't carry an unequipped " + def->Name);
        for (auto& it : npc.Inventory) {                   // the slot's current item comes off
            if (!it.Equipped) continue;
            const ItemDef* other = db.Item(it.Item);
            if (other && other->Slot == def->Slot) it.Equipped = false;
        }
        target->Equipped = true;
        out = Apply(npc, db, item, EffectTrigger::Equip);
        return true;
    }

    std::vector<NpcItem> Die(NpcComponent& npc) {
        npc.Dead = true;
        npc.Effects.clear();
        std::vector<NpcItem> drops;
        for (auto it = npc.Inventory.begin(); it != npc.Inventory.end();) {
            if (it->Drop) { drops.push_back(*it); drops.back().Equipped = false; it = npc.Inventory.erase(it); }
            else ++it;
        }
        return drops;
    }

    // ---------------------------------------------------------------- entities

    namespace {
        bool IsPrefabPath(const std::string& path) {
            return path.size() > 5 && path.compare(path.size() - 5, 5, ".json") == 0;
        }

        // Model geometry on an existing Mesh Renderer, scaled to `height` and standing on the origin
        bool SetModel(Entity e, const std::string& path, float height, const ResourceRegistry& res) {
            auto g = res.FindGeometry ? res.FindGeometry("model:" + path) : nullptr;
            auto* model = dynamic_cast<Model*>(g.get());
            if (!model) return false;
            auto& mr = e.Has<MeshRendererComponent>() ? e.Get<MeshRendererComponent>() : e.Add<MeshRendererComponent>();
            mr.Geometry = g;
            mr.LocalOffset = model->GetPlacementMatrix(std::max(0.01f, height));
            mr.UseModelMaterials = model->HasTexturedMaterials();
            if (mr.UseModelMaterials) mr.Color = glm::vec3(1.0f);
            if (model->IsSkinned() && !e.Has<AnimatorComponent>()) {
                auto& an = e.Add<AnimatorComponent>();
                if (!model->AnimationNames().empty()) an.Clip = model->AnimationNames().front();
            }
            return true;
        }
    }

    entt::entity MakeVisual(Scene& scene, const std::string& path, float height, const ResourceRegistry& res,
                            PrefabLibrary* prefabs, const std::string& name) {
        if (path.empty()) return entt::null;
        if (IsPrefabPath(path)) {
            if (!prefabs) return entt::null;
            Entity e = prefabs->Instantiate(scene, path);
            if (!e) return entt::null;
            if (!name.empty()) e.Get<TagComponent>().Tag = name;
            return e.GetHandle();
        }
        Entity e = scene.CreateEntity(name);
        if (!SetModel(e, path, height, res)) { scene.DestroyEntity(e); return entt::null; }
        return e.GetHandle();
    }

    void ApplyLook(Scene& scene, entt::entity handle, const NpcDef& def, const ResourceRegistry& res) {
        Entity e(handle, &scene);
        if (!def.Model.empty() && !IsPrefabPath(def.Model)) SetModel(e, def.Model, def.Height, res);
        if (!def.AnimGraph.empty()) {
            auto& an = e.Has<AnimatorComponent>() ? e.Get<AnimatorComponent>() : e.Add<AnimatorComponent>();
            an.Graph = def.AnimGraph;
            an.GraphData = res.LoadAnimGraph ? res.LoadAnimGraph(def.AnimGraph) : nullptr;
        }
        if (!def.Script.empty()) {
            if (e.Has<ScriptComponent>()) e.Get<ScriptComponent>().Path = def.Script;
            else e.Add<ScriptComponent>().Path = def.Script;
        }
        if (def.Collider) {
            // a kinematic capsule standing on the origin (an existing body of another kind is left as it is)
            RigidBodyComponent* rb = e.Has<RigidBodyComponent>() ? &e.Get<RigidBodyComponent>() : nullptr;
            if (!rb) { rb = &e.Add<RigidBodyComponent>(); rb->Type = BodyType::Kinematic; rb->Shape = ColliderShape::Capsule; }
            if (rb->Shape == ColliderShape::Capsule) {
                const float h = std::max(def.Height, def.Radius * 2.0f);
                rb->Size = glm::vec3(def.Radius * 2.0f, h, 0.0f);
                rb->Center = glm::vec3(0.0f, h * 0.5f, 0.0f);
            }
        }
    }

    entt::entity MakeNpc(Scene& scene, const GameDatabase& db, const std::string& id, const ResourceRegistry& res,
                         PrefabLibrary* prefabs) {
        const NpcDef* def = db.Npc(id);
        const std::string name = def && !def->Name.empty() ? def->Name : id;
        entt::entity e = def ? MakeVisual(scene, def->Model, def->Height, res, prefabs, name) : entt::null;
        if (e == entt::null) e = scene.CreateEntity(name).GetHandle();
        scene.Registry().emplace_or_replace<NpcComponent>(e, NpcComponent{ id });
        if (def) ApplyLook(scene, e, *def, res);
        return e;
    }

    entt::entity MakePickup(Scene& scene, const GameDatabase& db, const std::string& item, int count,
                            const glm::vec3& position, const ResourceRegistry& res, PrefabLibrary* prefabs) {
        const ItemDef* def = db.Item(item);
        const std::string name = def && !def->Name.empty() ? def->Name : item;
        entt::entity h = def ? MakeVisual(scene, def->Model, def->ModelHeight, res, prefabs, name) : entt::null;
        if (h == entt::null) {                             // no model: a small gold box
            Entity e = scene.CreateEntity(name);
            auto& mr = e.Add<MeshRendererComponent>();
            mr.Geometry = res.FindGeometry ? res.FindGeometry("cube") : nullptr;
            mr.LocalOffset = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.15f, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.3f));
            mr.Color = glm::vec3(0.95f, 0.75f, 0.25f);
            mr.Metallic = 0.6f;
            mr.Roughness = 0.35f;
            h = e.GetHandle();
        }
        auto& reg = scene.Registry();
        reg.emplace_or_replace<PickupComponent>(h, PickupComponent{ item, std::max(1, count) });
        reg.emplace_or_replace<SpinComponent>(h, SpinComponent{ glm::vec3(0.0f, 1.2f, 0.0f) });
        scene.SetWorldPosition(h, position);
        return h;
    }

} // namespace Engine::Characters
