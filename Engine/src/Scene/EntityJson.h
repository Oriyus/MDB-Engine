#pragma once
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

// Lets nlohmann/json read and write glm vectors as [x, y, z] and matrices as 16 floats (column-major)
namespace glm {
    inline void to_json(nlohmann::json& j, const vec3& v) { j = nlohmann::json::array({ v.x, v.y, v.z }); }
    inline void from_json(const nlohmann::json& j, vec3& v) {
        v = vec3(j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>());
    }
    inline void to_json(nlohmann::json& j, const mat4& m) {
        j = nlohmann::json::array();
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) j.push_back(m[c][r]);
    }
    inline void from_json(const nlohmann::json& j, mat4& m) {
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) m[c][r] = j.at(c * 4 + r).get<float>();
    }
}

namespace Engine {

    class Entity;
    struct ResourceRegistry;
    struct TransformComponent;

    // The one place that turns components into JSON and back. Scene files and prefab files both
    // use it. Adding a component type: add it to all three functions below.

    // Every component except Tag, Transform and Prefab, one key each ("meshRenderer", "rigidBody", ...)
    nlohmann::json ComponentsToJson(Entity e, const ResourceRegistry& resources);

    // Add (or replace) the components found in j. Unknown keys are ignored. Throws on malformed values.
    void ComponentsFromJson(const nlohmann::json& j, Entity e, const ResourceRegistry& resources);

    // Remove every component except Tag, Transform and Prefab
    void ClearComponents(Entity e);

    // True if j has any component key (anything besides name / transform / prefab)
    bool HasComponentKeys(const nlohmann::json& j);

    nlohmann::json TransformToJson(const TransformComponent& t);
    void TransformFromJson(const nlohmann::json& j, TransformComponent& t);

} // namespace Engine
