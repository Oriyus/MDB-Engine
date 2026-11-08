#include "Navigation/FogOfWar.h"
#include "Navigation/NavGrid.h"
#include "Physics/PhysicsWorld.h"
#include "Scene/Fog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Engine {

    void FogOfWar::Enable(const NavGrid& grid, FogOfWarState& state) {
        const bool sameSize = m_W == grid.GridWidth() && m_D == grid.GridDepth()
            && m_Origin == grid.GridOrigin() && m_Cell == grid.Settings().CellSize;
        m_Origin = grid.GridOrigin();
        m_Cell = grid.Settings().CellSize;
        m_W = grid.GridWidth();
        m_D = grid.GridDepth();
        m_GridBake = grid.BakeCount();
        m_Cache.clear();
        if (!sameSize) {
            m_Visible.assign(size_t(m_W) * m_D, 0);
            m_Explored.assign(size_t(m_W) * m_D, 0);
            m_ShownVisible.assign(size_t(m_W) * m_D, 0.0f);
            m_ShownExplored.assign(size_t(m_W) * m_D, 0.0f);
        }
        m_Enabled = true;
        state.Enabled = true;
        state.Origin = m_Origin;
        state.CellSize = m_Cell;
        state.Width = m_W;
        state.Depth = m_D;
        state.Texels.assign(size_t(m_W) * m_D * 2, 0);
        state.Revision++;
    }

    void FogOfWar::Disable(FogOfWarState& state) {
        m_Enabled = false;
        state.Enabled = false;
    }

    int FogOfWar::ColumnAt(float x, float z) const {
        const int ix = (int)std::floor((x - m_Origin.x) / m_Cell), iz = (int)std::floor((z - m_Origin.y) / m_Cell);
        if (ix < 0 || iz < 0 || ix >= m_W || iz >= m_D) return -1;
        return iz * m_W + ix;
    }

    void FogOfWar::BeginFrame() {
        std::fill(m_Visible.begin(), m_Visible.end(), uint8_t(0));
    }

    void FogOfWar::Reveal(const NavGrid& grid, const PhysicsWorld& physics, const glm::vec3& pos, float radius, float eyeHeight) {
        if (!m_Enabled || !grid.IsBaked()) return;
        if (grid.BakeCount() != m_GridBake) { m_GridBake = grid.BakeCount(); m_Cache.clear(); }
        radius = std::clamp(radius, 0.5f, 60.0f);

        // The cache key: the viewer's position in 0.5 m steps, its radius and eye height
        auto q = [](float v, float step) { return (uint64_t)((int64_t)std::floor(v / step) & 0xFFFF); };
        const uint64_t key = q(pos.x, 0.5f) | (q(pos.z, 0.5f) << 16) | (q(pos.y, 0.5f) << 32)
            | (q(radius, 0.25f) << 48) ^ (q(eyeHeight, 0.1f) << 56);
        auto it = m_Cache.find(key);
        if (it == m_Cache.end()) {
            const glm::vec3 viewer(std::floor(pos.x / 0.5f) * 0.5f + 0.25f, pos.y, std::floor(pos.z / 0.5f) * 0.5f + 0.25f);
            NavGrid::SightOptions o;
            o.EyeHeight = eyeHeight;
            o.TargetHeight = 1.0f;                     // as nav.line_of_sight: could a unit standing there be seen
            const int cx = (int)std::floor((viewer.x - m_Origin.x) / m_Cell), cz = (int)std::floor((viewer.z - m_Origin.y) / m_Cell);
            const int r = (int)std::ceil(radius / m_Cell) + 1;
            std::vector<uint8_t> seen(size_t(m_W) * m_D, 0);
            std::vector<int> inRange;
            for (int iz = std::max(cz - r, 0); iz <= std::min(cz + r, m_D - 1); iz++)
                for (int ix = std::max(cx - r, 0); ix <= std::min(cx + r, m_W - 1); ix++) {
                    const float dx = m_Origin.x + (ix + 0.5f) * m_Cell - viewer.x;
                    const float dz = m_Origin.y + (iz + 0.5f) * m_Cell - viewer.z;
                    if (dx * dx + dz * dz > radius * radius) continue;
                    const int column = iz * m_W + ix;
                    inRange.push_back(column);
                    int first, end;
                    grid.ColumnCells(ix, iz, first, end);
                    for (int c = first; c < end; c++) {
                        const glm::vec3 p = grid.CellPosition(c);
                        const bool near = dx * dx + dz * dz <= 2.25f && std::abs(p.y - pos.y) < 1.0f;
                        if (near || grid.LineOfSight(physics, pos, p, o)) { seen[column] = 1; break; }
                    }
                }
            // Columns with no floor (walls, the sides of crates) show when a seen floor is next to them
            std::vector<int> visible;
            for (int column : inRange) {
                const int ix = column % m_W, iz = column / m_W;
                int first, end;
                grid.ColumnCells(ix, iz, first, end);
                bool show = seen[column] != 0;
                if (!show && first == end)
                    for (int nz = std::max(iz - 1, 0); nz <= std::min(iz + 1, m_D - 1) && !show; nz++)
                        for (int nx = std::max(ix - 1, 0); nx <= std::min(ix + 1, m_W - 1) && !show; nx++)
                            show = seen[nz * m_W + nx] != 0;
                if (show) visible.push_back(column);
            }
            if (m_Cache.size() > 512) m_Cache.clear();
            it = m_Cache.emplace(key, std::move(visible)).first;
        }
        for (int column : it->second) m_Visible[column] = 1;
    }

    void FogOfWar::Explore(const glm::vec3& pos, float radius) {
        if (!m_Enabled) return;
        for (int iz = 0; iz < m_D; iz++)
            for (int ix = 0; ix < m_W; ix++) {
                const float dx = m_Origin.x + (ix + 0.5f) * m_Cell - pos.x, dz = m_Origin.y + (iz + 0.5f) * m_Cell - pos.z;
                if (dx * dx + dz * dz <= radius * radius) m_Explored[iz * m_W + ix] = 1;
            }
    }

    void FogOfWar::EndFrame(FogOfWarState& state, float dt) {
        if (!m_Enabled) return;
        const float step = FadeSpeed > 0.0f ? FadeSpeed * dt : 1.0f;
        bool changed = false;
        for (size_t i = 0; i < m_Visible.size(); i++) {
            if (m_Visible[i]) m_Explored[i] = 1;
            auto fade = [&](float& shown, float target) {
                if (shown == target) return;
                shown = shown < target ? std::min(target, shown + step) : std::max(target, shown - step);
                changed = true;
            };
            fade(m_ShownVisible[i], m_Visible[i] ? 1.0f : 0.0f);
            fade(m_ShownExplored[i], m_Explored[i] ? 1.0f : 0.0f);
            state.Texels[i * 2] = (uint8_t)std::lround(m_ShownVisible[i] * 255.0f);
            state.Texels[i * 2 + 1] = (uint8_t)std::lround(m_ShownExplored[i] * 255.0f);
        }
        if (changed) state.Revision++;
    }

    bool FogOfWar::IsVisible(const glm::vec3& pos) const {
        const int c = m_Enabled ? ColumnAt(pos.x, pos.z) : -1;
        return !m_Enabled || (c >= 0 && m_Visible[c] != 0);     // no fog: everything is visible
    }

    bool FogOfWar::IsExplored(const glm::vec3& pos) const {
        const int c = m_Enabled ? ColumnAt(pos.x, pos.z) : -1;
        return !m_Enabled || (c >= 0 && m_Explored[c] != 0);
    }

    void FogOfWar::Reset() {
        std::fill(m_Explored.begin(), m_Explored.end(), uint8_t(0));
        std::fill(m_ShownExplored.begin(), m_ShownExplored.end(), 0.0f);
    }

    // "fog1:<width>:<depth>:<hex of the explored bits, 8 columns per byte>"
    std::string FogOfWar::Save() const {
        std::string out = "fog1:" + std::to_string(m_W) + ":" + std::to_string(m_D) + ":";
        static const char* hex = "0123456789abcdef";
        for (size_t i = 0; i < m_Explored.size(); i += 8) {
            uint8_t byte = 0;
            for (size_t b = 0; b < 8 && i + b < m_Explored.size(); b++) if (m_Explored[i + b]) byte |= uint8_t(1u << b);
            out += hex[byte >> 4];
            out += hex[byte & 15];
        }
        return out;
    }

    bool FogOfWar::Load(const std::string& data) {
        int w = 0, d = 0, used = 0;
        if (std::sscanf(data.c_str(), "fog1:%d:%d:%n", &w, &d, &used) != 2 || w != m_W || d != m_D) return false;
        const std::string bits = data.substr((size_t)used);
        if (bits.size() < ((m_Explored.size() + 7) / 8) * 2) return false;
        auto nibble = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
        for (size_t i = 0; i < m_Explored.size(); i++) {
            const size_t byte = i / 8;
            const int value = nibble(bits[byte * 2]) * 16 + nibble(bits[byte * 2 + 1]);
            m_Explored[i] = (value >> (i % 8)) & 1;
            m_ShownExplored[i] = m_Explored[i] ? 1.0f : 0.0f;    // no fade-in after a load
        }
        return true;
    }

} // namespace Engine
