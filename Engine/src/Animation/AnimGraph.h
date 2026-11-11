#pragma once
#include "Renderer/Model.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    struct AnimatorComponent;

    // An animation graph (Assets/AnimGraphs/<name>.json, edited in the editor's AnimGraph Editor): parameters, and
    // layers that each run a state machine. An Animator with Graph set plays it instead of its Clip.
    //
    //   Parameters  float, int, bool or trigger (a bool that a transition using it resets). Scripts set them
    //               (e.animator:set("speed", 2), :trigger("jump")); the editor can too while playing.
    //   Layers      the first is the base pose; each further one replaces (or, Additive, adds to) the pose of its
    //               Mask (a bone and everything below it; "" = the whole skeleton) by Weight x WeightParam.
    //   States      play a clip, or a 1D blend space: clips placed along a parameter (walk at 1, run at 4), the two
    //               around its value blended and kept in step (one shared phase, so feet stay matched).
    //   Transitions from a state (or from Any State) to another when all conditions hold and, with an exit time,
    //               once the state's phase has passed it; the two states cross-fade over Duration seconds.
    //               Triggers in the conditions are reset when it is taken. Entering a state sends scripts
    //               on_anim_event(self, "enter", <state name>).
    enum class AnimParamType : int { Float = 0, Int = 1, Bool = 2, Trigger = 3 };
    struct AnimParam {
        std::string Name;
        AnimParamType Type = AnimParamType::Float;
        float Default = 0.0f;
    };

    // Greater / Less / Equal / NotEqual compare with Value; IsTrue / IsFalse test a bool or trigger (or a number != 0)
    enum class AnimCondOp : int { Greater = 0, Less = 1, Equal = 2, NotEqual = 3, IsTrue = 4, IsFalse = 5 };
    struct AnimCondition {
        std::string Param;
        AnimCondOp Op = AnimCondOp::Greater;
        float Value = 0.0f;
    };

    struct AnimBlendPoint {
        std::string Clip;
        float Threshold = 0.0f;          // where along the blend parameter this clip plays alone
        float Speed = 1.0f;              // playback speed of this clip at that point
    };

    enum class AnimStateKind : int { Clip = 0, Blend1D = 1 };
    struct AnimState {
        int Id = 0;                      // unique in its layer; transitions refer to it
        std::string Name = "State";
        AnimStateKind Kind = AnimStateKind::Clip;
        std::string Clip;                // Clip
        std::string BlendParam;          // Blend1D
        std::vector<AnimBlendPoint> Points;   // Blend1D, any order (sorted by threshold when used)
        float Speed = 1.0f;
        std::string SpeedParam;          // optional: the speed is also multiplied by this parameter
        bool Loop = true;
        glm::vec2 Pos{ 0.0f };           // editor canvas position
    };

    struct AnimTransition {
        int From = -1;                   // a state id, or AnyState
        int To = 0;
        float Duration = 0.25f;          // cross-fade seconds
        bool HasExitTime = false;
        float ExitTime = 1.0f;           // the From state's phase (0..1 of its length) it waits for
        std::vector<AnimCondition> Conditions;   // all must hold (none: just the exit time)
        static constexpr int AnyState = -1;
    };

    struct AnimGraphLayer {
        std::string Name = "Base";
        std::string Mask;                // a bone: it and its children ("" = everything); ignored on the base layer
        float Weight = 1.0f;
        std::string WeightParam;         // optional: weight x this parameter (0..1), so scripts can fade the layer
        bool Additive = false;           // add the motion relative to the state's first frame (not on the base layer)
        int Entry = 0;                   // the state it starts in
        std::vector<AnimState> States;
        std::vector<AnimTransition> Transitions;
        glm::vec2 EntryPos{ 40.0f, 80.0f }, AnyPos{ 40.0f, 200.0f };   // editor canvas positions

        const AnimState* Find(int id) const;
        AnimState* Find(int id);
        int NewId() const;
    };

    struct AnimGraph {
        std::vector<AnimParam> Params;
        std::vector<AnimGraphLayer> Layers;

        const AnimParam* FindParam(const std::string& name) const;
        nlohmann::json ToJson() const;
        static AnimGraph FromJson(const nlohmann::json& j);      // throws on malformed values
        static std::shared_ptr<AnimGraph> Load(const std::string& path, std::string* error = nullptr);
        bool Save(const std::string& path) const;
        // Mistakes worth showing (missing clips or parameters, unreachable states, transitions that never fire).
        // clips: the clips of the model it is meant for (null: not checked). where: "layer:state" per issue (optional).
        std::vector<std::string> Issues(const std::vector<std::string>* clips = nullptr) const;
    };

    // Advance a clip's time by `step` seconds and report the events it crossed into an.Fired (an Animator's events,
    // and "end" when a non-looping clip reaches its end). Looping clips wrap in either direction.
    float AdvanceClip(AnimatorComponent& an, const Model& model, int clip, const std::string& clipName,
                      float time, float step, bool loop);

    // What the base layer's leading clip did this frame (root motion follows it)
    struct AnimGraphBase {
        int Clip = -1;                   // the clip with the most weight in the base layer's current state
        float PrevTime = 0.0f, Time = 0.0f, Step = 0.0f;   // seconds in that clip
        bool Loop = true;
    };

    // Run an.GraphData for dt seconds (0: just pose it, as in Edit mode): take transitions, advance every layer,
    // and blend the layers into pose (one NodePose per model node). Fills in missing parameters with their
    // defaults and keeps an.GraphLayers (the runtime state) in step with the graph.
    void EvaluateAnimGraph(AnimatorComponent& an, const Model& model, float dt, std::vector<Model::NodePose>& pose,
                           AnimGraphBase& base);

    // The current state's name in layer `layer` ("" if none), for scripts and the editor
    std::string AnimGraphStateName(const AnimatorComponent& an, size_t layer);
    // Force a state (cross-fade over `blend` seconds). False if no state has that name.
    bool AnimGraphPlayState(AnimatorComponent& an, size_t layer, const std::string& state, float blend);

} // namespace Engine
