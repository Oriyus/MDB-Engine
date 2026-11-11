#include "Animation/AnimGraph.h"
#include "Scene/Components.h"
#include "Scene/EntityJson.h"     // glm <-> json
#include "Core/Log.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

using json = nlohmann::json;

namespace Engine {

    // ---------------------------------------------------------------- graph lookups
    const AnimState* AnimGraphLayer::Find(int id) const {
        for (const AnimState& s : States) if (s.Id == id) return &s;
        return nullptr;
    }
    AnimState* AnimGraphLayer::Find(int id) {
        for (AnimState& s : States) if (s.Id == id) return &s;
        return nullptr;
    }
    int AnimGraphLayer::NewId() const {
        int id = 1;
        for (const AnimState& s : States) id = std::max(id, s.Id + 1);
        return id;
    }
    const AnimParam* AnimGraph::FindParam(const std::string& name) const {
        for (const AnimParam& p : Params) if (p.Name == name) return &p;
        return nullptr;
    }

    // ---------------------------------------------------------------- JSON
    namespace {
        const char* TypeName(AnimParamType t) {
            return t == AnimParamType::Int ? "int" : t == AnimParamType::Bool ? "bool" : t == AnimParamType::Trigger ? "trigger" : "float";
        }
        AnimParamType TypeFrom(const std::string& s) {
            return s == "int" ? AnimParamType::Int : s == "bool" ? AnimParamType::Bool : s == "trigger" ? AnimParamType::Trigger : AnimParamType::Float;
        }
        const char* OpName(AnimCondOp o) {
            switch (o) {
            case AnimCondOp::Less: return "<";
            case AnimCondOp::Equal: return "==";
            case AnimCondOp::NotEqual: return "!=";
            case AnimCondOp::IsTrue: return "true";
            case AnimCondOp::IsFalse: return "false";
            default: return ">";
            }
        }
        AnimCondOp OpFrom(const std::string& s) {
            return s == "<" ? AnimCondOp::Less : s == "==" ? AnimCondOp::Equal : s == "!=" ? AnimCondOp::NotEqual
                 : s == "true" ? AnimCondOp::IsTrue : s == "false" ? AnimCondOp::IsFalse : AnimCondOp::Greater;
        }
    }

    json AnimGraph::ToJson() const {
        json j = { { "version", 1 } };
        json params = json::array();
        for (const AnimParam& p : Params) params.push_back({ { "name", p.Name },{ "type", TypeName(p.Type) },{ "default", p.Default } });
        j["params"] = params;
        json layers = json::array();
        for (const AnimGraphLayer& L : Layers) {
            json jl = { { "name", L.Name },{ "mask", L.Mask },{ "weight", L.Weight },{ "weightParam", L.WeightParam },
                        { "additive", L.Additive },{ "entry", L.Entry },{ "entryPos", L.EntryPos },{ "anyPos", L.AnyPos } };
            json states = json::array();
            for (const AnimState& s : L.States) {
                json js = { { "id", s.Id },{ "name", s.Name },{ "kind", s.Kind == AnimStateKind::Blend1D ? "blend1d" : "clip" },
                            { "speed", s.Speed },{ "speedParam", s.SpeedParam },{ "loop", s.Loop },{ "pos", s.Pos } };
                if (s.Kind == AnimStateKind::Clip) js["clip"] = s.Clip;
                else {
                    js["param"] = s.BlendParam;
                    json pts = json::array();
                    for (const AnimBlendPoint& p : s.Points) pts.push_back({ { "clip", p.Clip },{ "threshold", p.Threshold },{ "speed", p.Speed } });
                    js["points"] = pts;
                }
                states.push_back(js);
            }
            jl["states"] = states;
            json transitions = json::array();
            for (const AnimTransition& t : L.Transitions) {
                json jt = { { "to", t.To },{ "duration", t.Duration } };
                jt["from"] = t.From == AnimTransition::AnyState ? json("any") : json(t.From);
                jt["exitTime"] = t.HasExitTime ? json(t.ExitTime) : json(nullptr);
                json conds = json::array();
                for (const AnimCondition& c : t.Conditions) conds.push_back({ { "param", c.Param },{ "op", OpName(c.Op) },{ "value", c.Value } });
                jt["conditions"] = conds;
                transitions.push_back(jt);
            }
            jl["transitions"] = transitions;
            layers.push_back(jl);
        }
        j["layers"] = layers;
        return j;
    }

    AnimGraph AnimGraph::FromJson(const json& j) {
        AnimGraph g;
        for (const json& jp : j.value("params", json::array())) {
            AnimParam p;
            p.Name = jp.value("name", std::string());
            p.Type = TypeFrom(jp.value("type", std::string("float")));
            p.Default = jp.value("default", 0.0f);
            g.Params.push_back(p);
        }
        for (const json& jl : j.value("layers", json::array())) {
            AnimGraphLayer L;
            L.Name = jl.value("name", L.Name);
            L.Mask = jl.value("mask", std::string());
            L.Weight = jl.value("weight", 1.0f);
            L.WeightParam = jl.value("weightParam", std::string());
            L.Additive = jl.value("additive", false);
            L.Entry = jl.value("entry", 0);
            if (jl.contains("entryPos")) L.EntryPos = jl["entryPos"].get<glm::vec2>();
            if (jl.contains("anyPos")) L.AnyPos = jl["anyPos"].get<glm::vec2>();
            for (const json& js : jl.value("states", json::array())) {
                AnimState s;
                s.Id = js.value("id", 0);
                s.Name = js.value("name", s.Name);
                s.Kind = js.value("kind", std::string("clip")) == "blend1d" ? AnimStateKind::Blend1D : AnimStateKind::Clip;
                s.Clip = js.value("clip", std::string());
                s.BlendParam = js.value("param", std::string());
                for (const json& jp : js.value("points", json::array()))
                    s.Points.push_back({ jp.value("clip", std::string()), jp.value("threshold", 0.0f), jp.value("speed", 1.0f) });
                s.Speed = js.value("speed", 1.0f);
                s.SpeedParam = js.value("speedParam", std::string());
                s.Loop = js.value("loop", true);
                if (js.contains("pos")) s.Pos = js["pos"].get<glm::vec2>();
                L.States.push_back(s);
            }
            for (const json& jt : jl.value("transitions", json::array())) {
                AnimTransition t;
                const json& from = jt.contains("from") ? jt["from"] : json("any");
                t.From = from.is_number() ? from.get<int>() : AnimTransition::AnyState;
                t.To = jt.value("to", 0);
                t.Duration = jt.value("duration", 0.25f);
                if (jt.contains("exitTime") && jt["exitTime"].is_number()) { t.HasExitTime = true; t.ExitTime = jt["exitTime"].get<float>(); }
                for (const json& jc : jt.value("conditions", json::array()))
                    t.Conditions.push_back({ jc.value("param", std::string()), OpFrom(jc.value("op", std::string(">"))), jc.value("value", 0.0f) });
                L.Transitions.push_back(t);
            }
            g.Layers.push_back(std::move(L));
        }
        return g;
    }

    std::shared_ptr<AnimGraph> AnimGraph::Load(const std::string& path, std::string* error) {
        std::ifstream f(std::filesystem::u8path(path));
        if (!f) { if (error) *error = "can't open the file"; return nullptr; }
        try {
            json j;
            f >> j;
            return std::make_shared<AnimGraph>(FromJson(j));
        }
        catch (const std::exception& e) {
            if (error) *error = e.what();
            return nullptr;
        }
    }

    bool AnimGraph::Save(const std::string& path) const {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(path).parent_path(), ec);
        std::ofstream f(std::filesystem::u8path(path), std::ios::trunc);
        if (!f) { ENGINE_ERROR("AnimGraph: can't write {}", path); return false; }
        f << ToJson().dump(2);
        return (bool)f;
    }

    std::vector<std::string> AnimGraph::Issues(const std::vector<std::string>* clips) const {
        std::vector<std::string> out;
        auto clipOk = [&](const std::string& c) { return !clips || std::find(clips->begin(), clips->end(), c) != clips->end(); };
        auto paramOk = [&](const std::string& n) { return n.empty() || FindParam(n) != nullptr; };
        std::set<std::string> names;
        for (const AnimParam& p : Params) {
            if (p.Name.empty()) out.push_back("A parameter has no name");
            else if (!names.insert(p.Name).second) out.push_back("Two parameters are called \"" + p.Name + "\"");
        }
        if (Layers.empty()) out.push_back("The graph has no layers");
        for (const AnimGraphLayer& L : Layers) {
            const std::string at = L.Name + ": ";
            if (L.States.empty()) { out.push_back(at + "no states"); continue; }
            if (!L.Find(L.Entry)) out.push_back(at + "the entry state is missing");
            if (!paramOk(L.WeightParam)) out.push_back(at + "no parameter \"" + L.WeightParam + "\" (weight)");
            for (const AnimState& s : L.States) {
                const std::string where = at + s.Name + ": ";
                if (s.Kind == AnimStateKind::Clip) {
                    if (s.Clip.empty()) out.push_back(where + "no clip");
                    else if (!clipOk(s.Clip)) out.push_back(where + "the model has no clip \"" + s.Clip + "\"");
                }
                else {
                    if (s.BlendParam.empty()) out.push_back(where + "no blend parameter");
                    else if (!paramOk(s.BlendParam)) out.push_back(where + "no parameter \"" + s.BlendParam + "\"");
                    if (s.Points.size() < 2) out.push_back(where + "a blend needs two clips or more");
                    for (const AnimBlendPoint& p : s.Points)
                        if (!p.Clip.empty() && !clipOk(p.Clip)) out.push_back(where + "the model has no clip \"" + p.Clip + "\"");
                }
                if (!paramOk(s.SpeedParam)) out.push_back(where + "no parameter \"" + s.SpeedParam + "\" (speed)");
            }
            // Reachable from the entry, or from Any State
            std::set<int> seen;
            std::vector<int> todo = { L.Entry };
            for (const AnimTransition& t : L.Transitions) if (t.From == AnimTransition::AnyState) todo.push_back(t.To);
            while (!todo.empty()) {
                const int s = todo.back(); todo.pop_back();
                if (!seen.insert(s).second) continue;
                for (const AnimTransition& t : L.Transitions) if (t.From == s) todo.push_back(t.To);
            }
            for (const AnimState& s : L.States)
                if (!seen.count(s.Id)) out.push_back(at + s.Name + " can't be reached");
            for (const AnimTransition& t : L.Transitions) {
                const AnimState* from = L.Find(t.From);
                const AnimState* to = L.Find(t.To);
                const std::string name = at + (t.From == AnimTransition::AnyState ? std::string("Any State") : from ? from->Name : std::string("?")) +
                                         " -> " + (to ? to->Name : std::string("?")) + ": ";
                if (!to || (t.From != AnimTransition::AnyState && !from)) { out.push_back(name + "links a missing state"); continue; }
                if (t.Conditions.empty() && !t.HasExitTime) out.push_back(name + "no conditions and no exit time: never taken");
                if (t.Conditions.empty() && t.From == AnimTransition::AnyState) out.push_back(name + "from Any State without conditions");
                for (const AnimCondition& c : t.Conditions)
                    if (!FindParam(c.Param)) out.push_back(name + "no parameter \"" + c.Param + "\"");
            }
        }
        return out;
    }

    // ---------------------------------------------------------------- clip time (shared with Scene.cpp)
    float AdvanceClip(AnimatorComponent& an, const Model& model, int clip, const std::string& clipName,
        float time, float step, bool loop) {
        const float d = model.AnimationDuration(clip);
        if (d <= 0.0f) return 0.0f;
        if (step == 0.0f) return loop ? time : std::clamp(time, 0.0f, d);

        // Fire every event of this clip with a time in the half-open range (a, b]
        auto fire = [&](float a, float b) {
            for (const auto& ev : an.Events)
                if (ev.Clip == clipName && ev.Time > a && ev.Time <= b) an.Fired.emplace_back(ev.Name, clipName);
        };
        const float raw = time + step;
        if (!loop) {
            const float t = std::clamp(raw, 0.0f, d);
            if (step > 0.0f) {
                fire(time, t);
                if (time < d && t >= d) an.Fired.emplace_back("end", clipName);
            }
            else {
                fire(t - 1e-6f, time - 1e-6f);   // backwards: crossing from above
                if (time > 0.0f && t <= 0.0f) an.Fired.emplace_back("end", clipName);
            }
            return t;
        }
        // Looping: walk the range one lap at a time (a big step can cross several laps)
        float t = raw;
        if (step > 0.0f) {
            float from = time;
            int guard = 0;
            while (t > d && guard++ < 8) { fire(from, d); from = -1e-6f; t -= d; }   // -1e-6: an event at 0 fires on wrap
            fire(from, t);
        }
        else {
            float to = time;
            int guard = 0;
            while (t < 0.0f && guard++ < 8) { fire(-1e-6f, to - 1e-6f); to = d + 1e-6f; t += d; }
            fire(t - 1e-6f, to - 1e-6f);
        }
        t = std::fmod(t, d);
        return t < 0.0f ? t + d : t;
    }

    // ---------------------------------------------------------------- runtime
    namespace {
        struct ClipSample {
            int Clip = -1;
            const std::string* Name = nullptr;
            float Weight = 0.0f, Duration = 0.0f, Speed = 1.0f;
        };

        float ParamValue(const AnimatorComponent& an, const AnimGraph& g, const std::string& name, float fallback) {
            if (name.empty()) return fallback;
            if (auto it = an.Params.find(name); it != an.Params.end()) return it->second;
            const AnimParam* p = g.FindParam(name);
            return p ? p->Default : fallback;
        }

        // The clips a state plays now and their weights: one for a clip state, the two around the parameter's value
        // for a 1D blend (one past either end)
        void StateClips(const AnimState& s, const AnimGraph& g, const AnimatorComponent& an, const Model& m,
                        std::vector<ClipSample>& out) {
            out.clear();
            auto add = [&](const std::string& clip, float w, float speed) {
                ClipSample c;
                c.Clip = m.FindAnimation(clip);
                c.Name = &clip;
                c.Weight = w;
                c.Duration = m.AnimationDuration(c.Clip);
                c.Speed = speed;
                out.push_back(c);
            };
            if (s.Kind == AnimStateKind::Clip) { add(s.Clip, 1.0f, 1.0f); return; }
            if (s.Points.empty()) return;
            static thread_local std::vector<const AnimBlendPoint*> pts;
            pts.clear();
            for (const AnimBlendPoint& p : s.Points) pts.push_back(&p);
            std::stable_sort(pts.begin(), pts.end(), [](const AnimBlendPoint* a, const AnimBlendPoint* b) { return a->Threshold < b->Threshold; });
            const float v = ParamValue(an, g, s.BlendParam, 0.0f);
            if (v <= pts.front()->Threshold) { add(pts.front()->Clip, 1.0f, pts.front()->Speed); return; }
            if (v >= pts.back()->Threshold) { add(pts.back()->Clip, 1.0f, pts.back()->Speed); return; }
            for (size_t i = 0; i + 1 < pts.size(); i++) {
                const float a = pts[i]->Threshold, b = pts[i + 1]->Threshold;
                if (v < a || v > b) continue;
                const float t = b - a > 1e-6f ? (v - a) / (b - a) : 0.0f;
                if (t < 1.0f) add(pts[i]->Clip, 1.0f - t, pts[i]->Speed);
                if (t > 0.0f) add(pts[i + 1]->Clip, t, pts[i + 1]->Speed);
                return;
            }
        }

        // Seconds for one pass through the state at speed 1: the clips' lengths (at their own speeds) by weight
        float CycleLength(const std::vector<ClipSample>& clips) {
            float len = 0.0f;
            for (const ClipSample& c : clips)
                if (c.Duration > 0.0f) len += c.Weight * c.Duration / std::max(std::abs(c.Speed), 1e-3f);
            return len;
        }

        size_t Dominant(const std::vector<ClipSample>& clips) {
            size_t best = 0;
            for (size_t i = 1; i < clips.size(); i++) if (clips[i].Weight > clips[best].Weight) best = i;
            return best;
        }

        // pose = the clips at `phase`, blended by weight (a missing clip is the rest pose)
        void SampleState(const std::vector<ClipSample>& clips, const Model& m, float phase,
                         std::vector<Model::NodePose>& pose, std::vector<Model::NodePose>& tmp) {
            if (clips.empty()) { m.SamplePose(-1, 0.0f, pose); return; }
            m.SamplePose(clips[0].Clip, phase * clips[0].Duration, pose);
            float acc = clips[0].Weight;
            for (size_t k = 1; k < clips.size(); k++) {
                const float w = clips[k].Weight;
                if (w <= 0.0f) continue;
                m.SamplePose(clips[k].Clip, phase * clips[k].Duration, tmp);
                const float t = w / std::max(acc + w, 1e-6f);
                for (size_t i = 0; i < pose.size() && i < tmp.size(); i++) {
                    pose[i].T = glm::mix(pose[i].T, tmp[i].T, t);
                    pose[i].R = glm::slerp(pose[i].R, tmp[i].R, t);
                    pose[i].S = glm::mix(pose[i].S, tmp[i].S, t);
                }
                acc += w;
            }
        }

        void Blend(std::vector<Model::NodePose>& a, const std::vector<Model::NodePose>& b, float t, const std::vector<uint8_t>* mask) {
            for (size_t i = 0; i < a.size() && i < b.size(); i++) {
                if (mask && !(*mask)[i]) continue;
                a[i].T = glm::mix(a[i].T, b[i].T, t);
                a[i].R = glm::slerp(a[i].R, b[i].R, t);
                a[i].S = glm::mix(a[i].S, b[i].S, t);
            }
        }

        bool Holds(const AnimCondition& c, const AnimatorComponent& an, const AnimGraph& g) {
            const AnimParam* p = g.FindParam(c.Param);
            if (!p) return false;
            const float v = ParamValue(an, g, c.Param, p->Default);
            switch (c.Op) {
            case AnimCondOp::Greater:  return v > c.Value;
            case AnimCondOp::Less:     return v < c.Value;
            case AnimCondOp::Equal:    return std::abs(v - c.Value) < 1e-4f;
            case AnimCondOp::NotEqual: return std::abs(v - c.Value) >= 1e-4f;
            case AnimCondOp::IsTrue:   return v > 0.5f;
            case AnimCondOp::IsFalse:  return v <= 0.5f;
            }
            return false;
        }

        void Enter(AnimatorComponent& an, AnimGraphLayerState& rt, const AnimState& to, float duration, int transition, bool instant) {
            rt.From = rt.State;
            rt.FromPhase = rt.Phase;
            rt.State = to.Id;
            rt.Phase = 0.0f;
            rt.StateTime = 0.0f;
            rt.FadeDuration = duration;
            rt.Transition = transition;
            rt.Fade = (instant || duration <= 0.0f || rt.From < 0) ? 1.0f : 0.0f;
            if (rt.Fade >= 1.0f) { rt.From = -1; rt.Transition = -1; }
            an.Fired.emplace_back("enter", to.Name);
        }
    }

    void EvaluateAnimGraph(AnimatorComponent& an, const Model& model, float dt, std::vector<Model::NodePose>& pose,
                           AnimGraphBase& base) {
        static thread_local std::vector<Model::NodePose> layerPose, fromPose, ref, tmp;
        static thread_local std::vector<uint8_t> mask;
        static thread_local std::vector<ClipSample> clips, fromClips;
        const AnimGraph& g = *an.GraphData;
        base = AnimGraphBase{};

        for (const AnimParam& p : g.Params)                     // parameters the graph gained
            if (!an.Params.count(p.Name)) an.Params[p.Name] = p.Default;
        if (an.GraphLayers.size() != g.Layers.size()) an.GraphLayers.resize(g.Layers.size());

        model.SamplePose(-1, 0.0f, pose);                       // a graph without layers: the rest pose
        const bool advancing = an.Playing && dt != 0.0f;

        for (size_t li = 0; li < g.Layers.size(); li++) {
            const AnimGraphLayer& L = g.Layers[li];
            AnimGraphLayerState& rt = an.GraphLayers[li];
            if (L.States.empty()) continue;

            // Not started, or its state was deleted (a live edit): the entry state
            if (!L.Find(rt.State)) {
                const AnimState* entry = L.Find(L.Entry);
                if (!entry) entry = &L.States.front();
                rt.State = -1;
                Enter(an, rt, *entry, 0.0f, -1, true);
            }
            if (rt.From >= 0 && !L.Find(rt.From)) { rt.From = -1; rt.Fade = 1.0f; rt.Transition = -1; }
            const AnimState& state = *L.Find(rt.State);

            // ---- advance the state (and the one fading out)
            StateClips(state, g, an, model, clips);
            const float speed = an.Speed * state.Speed * ParamValue(an, g, state.SpeedParam, 1.0f);
            const float step = advancing ? dt * speed : 0.0f;
            bool wrapped = false;
            if (!clips.empty()) {
                const float cycle = CycleLength(clips);
                const ClipSample& dom = clips[Dominant(clips)];
                const float phaseStep = cycle > 0.0f ? step / cycle : 0.0f;
                const float prevPhase = rt.Phase;
                if (dom.Duration > 0.0f) {
                    // The leading clip runs the phase: its events fire, a one-shot ends ("end")
                    const float prevSec = rt.Phase * dom.Duration;
                    const float domStep = phaseStep * dom.Duration;
                    const float sec = AdvanceClip(an, model, dom.Clip, *dom.Name, prevSec, domStep, state.Loop);
                    rt.Phase = std::clamp(sec / dom.Duration, 0.0f, 1.0f);
                    if (li == 0) base = { dom.Clip, prevSec, sec, domStep, state.Loop };
                }
                wrapped = state.Loop && phaseStep > 0.0f && rt.Phase < prevPhase;
            }
            if (advancing) rt.StateTime += std::abs(dt);

            const AnimState* from = rt.From >= 0 ? L.Find(rt.From) : nullptr;
            if (from && rt.Fade < 1.0f) {
                StateClips(*from, g, an, model, fromClips);
                const float fcycle = CycleLength(fromClips);
                const float fstep = advancing ? dt * an.Speed * from->Speed * ParamValue(an, g, from->SpeedParam, 1.0f) : 0.0f;
                float ph = rt.FromPhase + (fcycle > 0.0f ? fstep / fcycle : 0.0f);
                if (from->Loop) { ph = std::fmod(ph, 1.0f); if (ph < 0.0f) ph += 1.0f; }
                else ph = std::clamp(ph, 0.0f, 1.0f);
                rt.FromPhase = ph;
                rt.Fade = dt == 0.0f ? 1.0f : std::min(1.0f, rt.Fade + std::abs(dt) / std::max(rt.FadeDuration, 1e-4f));
            }
            else rt.Fade = 1.0f;

            // ---- this layer's pose (sampled before any transition below: a new state starts next frame)
            SampleState(clips, model, rt.Phase, layerPose, tmp);
            if (from && rt.Fade < 1.0f) {
                SampleState(fromClips, model, rt.FromPhase, fromPose, tmp);
                Blend(fromPose, layerPose, rt.Fade, nullptr);
                layerPose.swap(fromPose);
            }
            if (rt.Fade >= 1.0f) { rt.From = -1; rt.Transition = -1; }

            // ---- transitions: Any State first, then the current state's, in list order. While fading, only Any State.
            const bool fading = rt.Fade < 1.0f;
            for (int pass = 0; pass < 2; pass++) {
                bool taken = false;
                for (size_t ti = 0; ti < L.Transitions.size(); ti++) {
                    const AnimTransition& t = L.Transitions[ti];
                    const bool any = t.From == AnimTransition::AnyState;
                    if ((pass == 0) != any) continue;
                    if (!any && (fading || t.From != rt.State)) continue;
                    if (any && t.To == rt.State) continue;
                    const AnimState* to = L.Find(t.To);
                    if (!to) continue;
                    if (t.Conditions.empty() && !t.HasExitTime) continue;          // never taken (see Issues)
                    if (t.HasExitTime) {
                        const float exit = std::clamp(t.ExitTime, 0.0f, 1.0f);
                        if (!(rt.Phase >= exit || (wrapped && exit >= 0.999f))) continue;
                    }
                    bool ok = true;
                    for (const AnimCondition& c : t.Conditions) if (!Holds(c, an, g)) { ok = false; break; }
                    if (!ok) continue;
                    for (const AnimCondition& c : t.Conditions)                    // triggers are used up
                        if (const AnimParam* p = g.FindParam(c.Param); p && p->Type == AnimParamType::Trigger) an.Params[c.Param] = 0.0f;
                    Enter(an, rt, *to, t.Duration, (int)ti, dt == 0.0f);
                    taken = true;
                    break;
                }
                if (taken) break;
            }

            // ---- into the result
            if (li == 0) { pose.swap(layerPose); continue; }
            const float w = std::clamp(L.Weight * (L.WeightParam.empty() ? 1.0f : std::clamp(ParamValue(an, g, L.WeightParam, 1.0f), 0.0f, 1.0f)), 0.0f, 1.0f);
            if (w <= 0.0f) continue;
            int root = -1;
            if (!L.Mask.empty() && (root = model.FindNode(L.Mask)) < 0) continue;   // an unknown bone masks nothing
            model.SubtreeMask(root, mask);
            if (!L.Additive) { Blend(pose, layerPose, w, &mask); continue; }
            // Additive: the change from the state's first frame (the same blend), scaled by the weight
            StateClips(state, g, an, model, clips);
            SampleState(clips, model, 0.0f, ref, tmp);
            const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
            for (size_t i = 0; i < pose.size() && i < layerPose.size(); i++) {
                if (!mask[i]) continue;
                pose[i].T += (layerPose[i].T - ref[i].T) * w;
                pose[i].R = glm::normalize(pose[i].R * glm::slerp(identity, glm::inverse(ref[i].R) * layerPose[i].R, w));
                pose[i].S *= glm::mix(glm::vec3(1.0f), layerPose[i].S / glm::max(ref[i].S, glm::vec3(1e-6f)), w);
            }
        }
    }

    std::string AnimGraphStateName(const AnimatorComponent& an, size_t layer) {
        if (!an.GraphData || layer >= an.GraphData->Layers.size() || layer >= an.GraphLayers.size()) return "";
        const AnimState* s = an.GraphData->Layers[layer].Find(an.GraphLayers[layer].State);
        return s ? s->Name : "";
    }

    bool AnimGraphPlayState(AnimatorComponent& an, size_t layer, const std::string& name, float blend) {
        if (!an.GraphData || layer >= an.GraphData->Layers.size()) return false;
        if (an.GraphLayers.size() != an.GraphData->Layers.size()) an.GraphLayers.resize(an.GraphData->Layers.size());
        for (const AnimState& s : an.GraphData->Layers[layer].States) {
            if (s.Name != name) continue;
            AnimGraphLayerState& rt = an.GraphLayers[layer];
            Enter(an, rt, s, blend, -1, rt.State < 0);
            return true;
        }
        return false;
    }

} // namespace Engine
