#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine {

    class NavGrid;
    class PhysicsWorld;
    struct FogOfWarState;

    // Fog of war on the nav grid's columns (one texel each). Every frame the game says what its viewers can
    // see (Reveal: the columns within a radius that a viewer has line of sight to, walls next to them
    // included); what is visible becomes explored. EndFrame fades the shown values towards that and writes
    // the texels SceneRenderer draws from. 2D: a column counts as seen if any of its floors is.
    // A viewer's result is cached by its position (0.5 m steps), so a standing viewer costs nothing.
    class FogOfWar {
    public:
        // Size the map to the grid and switch the drawing on (what was explored stays if the size matches)
        void Enable(const NavGrid& grid, FogOfWarState& state);
        void Disable(FogOfWarState& state);
        bool IsEnabled() const { return m_Enabled; }

        void BeginFrame();                         // nothing is visible until Reveal says so
        void Reveal(const NavGrid& grid, const PhysicsWorld& physics, const glm::vec3& pos, float radius, float eyeHeight);
        void Explore(const glm::vec3& pos, float radius);   // mark explored without sight (a map, a scouting spell)
        void EndFrame(FogOfWarState& state, float dt);      // explored |= visible, fade, write the texels

        bool IsVisible(const glm::vec3& pos) const;
        bool IsExplored(const glm::vec3& pos) const;
        void Reset();                              // forget everything explored
        std::string Save() const;                  // the explored map, as a short string (for on_save)
        bool Load(const std::string& data);        // false if it was made for a differently sized grid

        float FadeSpeed = 4.0f;                    // shown values move this much per second (0..1)

    private:
        int ColumnAt(float x, float z) const;      // -1 outside the map

        bool m_Enabled = false;
        glm::vec2 m_Origin{ 0.0f };
        float m_Cell = 1.0f;
        int m_W = 0, m_D = 0;
        uint32_t m_GridBake = 0;
        std::vector<uint8_t> m_Visible, m_Explored;          // per column, 0 or 1
        std::vector<float> m_ShownVisible, m_ShownExplored;  // faded towards those, 0..1
        std::unordered_map<uint64_t, std::vector<int>> m_Cache;   // viewer (quantised) -> visible columns
    };

} // namespace Engine
