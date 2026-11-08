#include "Navigation/NavGrid.h"
#include "Physics/PhysicsWorld.h"
#include "Core/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <queue>

namespace Engine {

    namespace {
        // 0 +X, 1 -X, 2 +Z, 3 -Z, then the diagonals
        constexpr int kDX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        constexpr int kDZ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
        constexpr int kOpposite[8] = { 1, 0, 3, 2, 7, 6, 5, 4 };
        constexpr int kMaxColumns = 1024;        // per axis

        int DirOf(int dx, int dz) {
            for (int d = 0; d < 8; d++)
                if (kDX[d] == dx && kDZ[d] == dz) return d;
            return -1;
        }

        // Scratch for searches, reused between queries (the grid is used from the main thread only)
        struct Search {
            std::vector<float> G;
            std::vector<int> Parent;
            std::vector<uint32_t> Seen, Closed;
            uint32_t Gen = 0;

            void Begin(size_t n) {
                if (G.size() != n) {
                    G.assign(n, 0.0f);
                    Parent.assign(n, -1);
                    Seen.assign(n, 0);
                    Closed.assign(n, 0);
                    Gen = 0;
                }
                if (++Gen == 0) {                 // wrapped: start the stamps over
                    std::fill(Seen.begin(), Seen.end(), 0);
                    std::fill(Closed.begin(), Closed.end(), 0);
                    Gen = 1;
                }
            }
            bool Visited(int c) const { return Seen[c] == Gen; }
            void Visit(int c, float g, int parent) { Seen[c] = Gen; G[c] = g; Parent[c] = parent; }
        };
        Search g_Search;

        struct Open {
            float F;
            int Cell;
            bool operator>(const Open& o) const { return F > o.F; }
        };
    }

    void NavGrid::Clear() {
        m_Baked = false;
        m_W = m_D = 0;
        m_ColumnStart.clear();
        m_Cells.clear();
        m_Blocked.clear();
    }

    bool NavGrid::Bake(const PhysicsWorld& physics, const NavSettings& settings) {
        const auto t0 = std::chrono::steady_clock::now();
        Clear();
        m_Settings = settings;
        NavSettings& s = m_Settings;
        s.CellSize = std::max(s.CellSize, 0.1f);
        s.AgentRadius = std::max(s.AgentRadius, 0.0f);
        s.AgentHeight = std::max(s.AgentHeight, 0.1f);
        s.MaxStep = std::max(s.MaxStep, 0.0f);
        const float cs = s.CellSize;

        glm::vec3 mn = s.BoundsMin, mx = s.BoundsMax;
        if (s.AutoBounds && !physics.StaticBounds(mn, mx)) {
            ENGINE_WARN("Navigation: nothing to bake (no static bodies)");
            return false;
        }
        // Columns line up with whole multiples of the cell size, so cell centres don't move with the bounds
        m_Origin = glm::vec2(std::floor(mn.x / cs) * cs, std::floor(mn.z / cs) * cs);
        m_W = (int)std::ceil((mx.x - m_Origin.x) / cs);
        m_D = (int)std::ceil((mx.z - m_Origin.y) / cs);
        if (m_W > kMaxColumns || m_D > kMaxColumns)
            ENGINE_WARN("Navigation: {} x {} columns is too many, clamped to {} per side", m_W, m_D, kMaxColumns);
        m_W = std::clamp(m_W, 1, kMaxColumns);
        m_D = std::clamp(m_D, 1, kMaxColumns);

        const float top = mx.y + 1.0f, span = mx.y - mn.y + 2.0f;
        const float cosSlope = std::cos(glm::radians(std::clamp(s.MaxSlope, 0.0f, 89.0f)));
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        const float lowRay = s.MaxStep + 0.05f, highRay = s.AgentHeight * 0.8f;
        RayHit hit;
        auto blockedRay = [&](const glm::vec3& o, const glm::vec3& d, float len) {
            return len > 0.0f && physics.Raycast(o, d, len, hit, entt::null, true);
        };

        // ---- 1. walkable surfaces: every upward-facing hit under each column centre
        m_ColumnStart.assign(size_t(m_W) * m_D + 1, 0);
        std::vector<RayHit> hits;
        std::vector<float> heights;
        for (int iz = 0; iz < m_D; iz++) {
            for (int ix = 0; ix < m_W; ix++) {
                const int column = iz * m_W + ix;
                m_ColumnStart[column] = (int)m_Cells.size();
                const float x = m_Origin.x + (ix + 0.5f) * cs, z = m_Origin.y + (iz + 0.5f) * cs;
                physics.RaycastAll(glm::vec3(x, top, z), -up, span, hits, true);
                heights.clear();
                for (const RayHit& h : hits) {                       // top down
                    if (h.Normal.y < cosSlope) continue;
                    const glm::vec3 p(x, h.Point.y, z);
                    bool dup = false;
                    for (float y : heights) dup |= std::abs(y - p.y) < 0.05f;
                    if (dup) continue;
                    if (blockedRay(p + up * 0.02f, up, s.AgentHeight - 0.02f)) continue;   // no head room
                    bool wall = false;                               // a wall closer than the agent radius
                    for (int k = 0; k < 8 && !wall; k++) {
                        const float a = k * 0.78539816f;
                        const glm::vec3 d(std::cos(a), 0.0f, std::sin(a));
                        wall = blockedRay(p + up * lowRay, d, s.AgentRadius)
                            || ((k & 1) == 0 && blockedRay(p + up * highRay, d, s.AgentRadius));
                    }
                    if (wall) continue;
                    heights.push_back(p.y);
                }
                std::sort(heights.begin(), heights.end());           // bottom to top
                for (float y : heights) {
                    Cell c;
                    c.Pos = glm::vec3(x, y, z);
                    c.Column = column;
                    m_Cells.push_back(c);
                }
            }
        }
        m_ColumnStart.back() = (int)m_Cells.size();

        // ---- 2. links: a step up or down, with nothing in the way at foot and chest height
        auto linkTo = [&](const Cell& a, int nx, int nz) -> int {
            if (nx < 0 || nz < 0 || nx >= m_W || nz >= m_D) return -1;
            const int column = nz * m_W + nx;
            int best = -1;
            float bestDy = s.MaxStep + 1e-4f;
            for (int i = m_ColumnStart[column]; i < m_ColumnStart[column + 1]; i++) {
                const float dy = std::abs(m_Cells[i].Pos.y - a.Pos.y);
                if (dy <= bestDy) { bestDy = dy; best = i; }
            }
            if (best < 0) return -1;
            const Cell& b = m_Cells[best];
            const float base = std::max(a.Pos.y, b.Pos.y);
            glm::vec3 d = b.Pos - a.Pos;
            d.y = 0.0f;
            const float len = glm::length(d);
            for (float h : { lowRay, highRay }) {
                const glm::vec3 o(a.Pos.x, base + h, a.Pos.z);
                if (blockedRay(o, d, len)) return -1;
            }
            return best;
        };
        auto symmetric = [&](int first, int last) {
            for (int i = 0; i < (int)m_Cells.size(); i++)
                for (int d = first; d <= last; d++) {
                    const int n = m_Cells[i].Link[d];
                    if (n >= 0 && m_Cells[n].Link[kOpposite[d]] != i) m_Cells[i].Link[d] = -1;
                }
        };
        for (Cell& c : m_Cells) {
            const int ix = c.Column % m_W, iz = c.Column / m_W;
            for (int d = 0; d < 4; d++) c.Link[d] = linkTo(c, ix + kDX[d], iz + kDZ[d]);
        }
        symmetric(0, 3);
        for (Cell& c : m_Cells) {
            const int ix = c.Column % m_W, iz = c.Column / m_W;
            for (int d = 4; d < 8; d++) {
                const int ox = kDX[d] > 0 ? 0 : 1, oz = kDZ[d] > 0 ? 2 : 3;   // no cutting corners
                c.Link[d] = (c.Link[ox] >= 0 && c.Link[oz] >= 0) ? linkTo(c, ix + kDX[d], iz + kDZ[d]) : -1;
            }
        }
        symmetric(4, 7);

        m_Blocked.assign(m_Cells.size(), 0);
        m_Baked = true;
        m_BakeCount++;
        m_BakeMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();

        int layered = 0;
        for (int c = 0; c + 1 < (int)m_ColumnStart.size(); c++) layered += m_ColumnStart[c + 1] - m_ColumnStart[c] > 1;
        ENGINE_INFO("Navigation: baked {} x {} columns of {:.2f} m, {} cells ({} columns with more than one layer) in {:.1f} ms",
            m_W, m_D, cs, m_Cells.size(), layered, m_BakeMs);
        return true;
    }

    glm::vec3 NavGrid::CellPosition(int cell) const {
        return cell >= 0 && cell < (int)m_Cells.size() ? m_Cells[cell].Pos : glm::vec3(0.0f);
    }

    int NavGrid::CellLayer(int cell) const {
        if (cell < 0 || cell >= (int)m_Cells.size()) return 0;
        return cell - m_ColumnStart[m_Cells[cell].Column];
    }

    int NavGrid::Neighbor(int cell, int dir) const {
        if (cell < 0 || cell >= (int)m_Cells.size() || dir < 0 || dir > 7) return -1;
        return m_Cells[cell].Link[dir];
    }

    bool NavGrid::ColumnOf(float x, float z, int& ix, int& iz) const {
        if (!m_Baked) return false;
        ix = (int)std::floor((x - m_Origin.x) / m_Settings.CellSize);
        iz = (int)std::floor((z - m_Origin.y) / m_Settings.CellSize);
        return ix >= 0 && iz >= 0 && ix < m_W && iz < m_D;
    }

    int NavGrid::CellInColumn(int ix, int iz, float y) const {
        const int column = iz * m_W + ix;
        int best = -1;
        for (int i = m_ColumnStart[column]; i < m_ColumnStart[column + 1]; i++) {
            const float cy = m_Cells[i].Pos.y;
            if (cy <= y + m_Settings.MaxStep + 0.05f && y - cy <= m_Settings.AgentHeight) best = i;   // the highest one
        }
        return best;
    }

    int NavGrid::CellAt(const glm::vec3& p) const {
        int ix, iz;
        return ColumnOf(p.x, p.z, ix, iz) ? CellInColumn(ix, iz, p.y) : -1;
    }

    int NavGrid::Nearest(const glm::vec3& p, float maxDistance, bool skipBlocked) const {
        if (!m_Baked) return -1;
        const float cs = m_Settings.CellSize;
        const int cx = (int)std::floor((p.x - m_Origin.x) / cs), cz = (int)std::floor((p.z - m_Origin.y) / cs);
        const int r = (int)std::ceil(maxDistance / cs);
        int best = -1;
        float bestScore = 1e30f;
        for (int iz = std::max(cz - r, 0); iz <= std::min(cz + r, m_D - 1); iz++)
            for (int ix = std::max(cx - r, 0); ix <= std::min(cx + r, m_W - 1); ix++) {
                const int column = iz * m_W + ix;
                for (int i = m_ColumnStart[column]; i < m_ColumnStart[column + 1]; i++) {
                    if (skipBlocked && m_Blocked[i]) continue;
                    const glm::vec3 d = m_Cells[i].Pos - p;
                    const float flat = d.x * d.x + d.z * d.z;
                    if (flat > maxDistance * maxDistance) continue;
                    const float score = flat + 4.0f * d.y * d.y;       // prefer the same floor
                    if (score < bestScore) { bestScore = score; best = i; }
                }
            }
        return best;
    }

    int NavGrid::StartCell(const glm::vec3& p) const {
        const int c = CellAt(p);
        return c >= 0 ? c : Nearest(p, m_Settings.CellSize * 1.5f, false);
    }

    void NavGrid::SetBlocked(int cell, bool blocked) {
        if (cell >= 0 && cell < (int)m_Blocked.size()) m_Blocked[cell] = blocked ? 1 : 0;
    }
    bool NavGrid::IsBlocked(int cell) const {
        return cell >= 0 && cell < (int)m_Blocked.size() && m_Blocked[cell];
    }
    void NavGrid::ClearBlocked() { std::fill(m_Blocked.begin(), m_Blocked.end(), uint8_t(0)); }

    // Walk the segment column by column through the links. The agent's sides (radius) must not
    // leave the walkable area either: the column under each side must be linked from the centre's cell.
    bool NavGrid::LineClearFrom(int cell, const glm::vec3& a, const glm::vec3& b, int goal) const {
        if (cell < 0) return false;
        const float cs = m_Settings.CellSize;
        glm::vec2 d(b.x - a.x, b.z - a.z);
        const float dist = glm::length(d);
        const glm::vec2 side = dist > 1e-5f ? glm::vec2(-d.y, d.x) / dist * m_Settings.AgentRadius : glm::vec2(0.0f);
        const int steps = std::max(1, (int)std::ceil(dist / (cs * 0.2f)));
        int cur = cell;
        int cx = m_Cells[cur].Column % m_W, cz = m_Cells[cur].Column / m_W;
        for (int k = 0; k <= steps; k++) {
            const glm::vec2 p = glm::vec2(a.x, a.z) + d * (float(k) / steps);
            int jx, jz;
            if (!ColumnOf(p.x, p.y, jx, jz)) return false;
            while (jx != cx || jz != cz) {
                const int dx = (jx > cx) - (jx < cx), dz = (jz > cz) - (jz < cz);
                const int n = m_Cells[cur].Link[DirOf(dx, dz)];
                if (n < 0 || (m_Blocked[n] && n != goal)) return false;
                cur = n;
                cx += dx;
                cz += dz;
            }
            for (float sgn : { -1.0f, 1.0f }) {
                const glm::vec2 q = p + side * sgn;
                int qx, qz;
                if (!ColumnOf(q.x, q.y, qx, qz)) return false;
                const int dx = std::clamp(qx - cx, -1, 1), dz = std::clamp(qz - cz, -1, 1);
                if ((dx || dz) && m_Cells[cur].Link[DirOf(dx, dz)] < 0) return false;
            }
        }
        return goal < 0 || cur == goal;
    }

    bool NavGrid::LineClear(const glm::vec3& a, const glm::vec3& b) const {
        if (!m_Baked) return false;
        const int start = StartCell(a), goal = CellAt(b);
        return start >= 0 && goal >= 0 && LineClearFrom(start, a, b, goal);
    }

    bool NavGrid::FindPath(const glm::vec3& from, const glm::vec3& to, const NavPathOptions& o, NavPath& path) const {
        path = NavPath{};
        if (!m_Baked || m_Cells.empty()) return false;
        const int start = StartCell(from);
        int goal = CellAt(to);
        if (goal < 0) goal = Nearest(to, m_Settings.CellSize * 1.5f, false);
        if (start < 0 || goal < 0) return false;
        if (m_Blocked[goal] && goal != start && !o.Closest) return false;

        const float diag = std::clamp(o.Diagonal, 1.0f, 2.0f);
        const int gx = m_Cells[goal].Column % m_W, gz = m_Cells[goal].Column / m_W;
        auto heuristic = [&](int c) {
            const int dx = std::abs(m_Cells[c].Column % m_W - gx), dz = std::abs(m_Cells[c].Column / m_W - gz);
            const int lo = std::min(dx, dz), hi = std::max(dx, dz);
            return float(hi - lo) + float(lo) * diag;
        };

        Search& s = g_Search;
        s.Begin(m_Cells.size());
        std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
        s.Visit(start, 0.0f, -1);
        open.push({ heuristic(start), start });
        int best = start;
        float bestH = heuristic(start);
        bool found = start == goal;
        while (!open.empty() && !found) {
            const Open top = open.top();
            open.pop();
            const int c = top.Cell;
            if (s.Closed[c] == s.Gen) continue;
            s.Closed[c] = s.Gen;
            const float h = heuristic(c);
            if (h < bestH || (h == bestH && s.G[c] < s.G[best])) { bestH = h; best = c; }
            if (c == goal) { found = true; break; }
            for (int d = 0; d < 8; d++) {
                const int n = m_Cells[c].Link[d];
                if (n < 0 || s.Closed[n] == s.Gen || m_Blocked[n]) continue;
                const float g = s.G[c] + (d < 4 ? 1.0f : diag);
                if (s.Visited(n) && g >= s.G[n]) continue;
                s.Visit(n, g, c);
                open.push({ g + heuristic(n), n });
            }
        }
        if (!found && !o.Closest) return false;
        const int end = found ? goal : best;

        for (int c = end; c >= 0; c = s.Parent[c]) path.Cells.push_back(c);
        std::reverse(path.Cells.begin(), path.Cells.end());
        path.Complete = found;

        // Costs along the cells, cut where they would pass MaxCost
        std::vector<float> costs(path.Cells.size(), 0.0f);
        for (size_t i = 1; i < path.Cells.size(); i++) {
            const int a = path.Cells[i - 1], b = path.Cells[i];
            const bool diagonal = m_Cells[a].Column % m_W != m_Cells[b].Column % m_W
                && m_Cells[a].Column / m_W != m_Cells[b].Column / m_W;
            costs[i] = costs[i - 1] + (diagonal ? diag : 1.0f);
            if (o.MaxCost > 0.0f && costs[i] > o.MaxCost + 1e-4f) {
                path.Cells.resize(i);
                costs.resize(i);
                path.Complete = false;
                break;
            }
        }
        path.Cost = costs.back();

        if (o.Grid) {
            for (int c : path.Cells) path.Points.push_back(m_Cells[c].Pos);
            path.Costs = costs;
            return true;
        }

        // Free movement: from the exact start to the exact goal, straightened wherever the grid allows
        std::vector<glm::vec3> raw;
        for (int c : path.Cells) raw.push_back(m_Cells[c].Pos);
        raw.front() = glm::vec3(from.x, m_Cells[path.Cells.front()].Pos.y, from.z);
        if (path.Complete) raw.back() = glm::vec3(to.x, m_Cells[path.Cells.back()].Pos.y, to.z);
        const size_t last = raw.size() - 1;
        path.Points.push_back(raw[0]);
        size_t i = 0;
        while (i < last) {
            size_t j = last;
            while (j > i + 1 && !LineClearFrom(path.Cells[i], raw[i], raw[j], path.Cells[j])) j--;
            path.Points.push_back(raw[j]);
            i = j;
        }
        if (path.Points.size() >= 2 && glm::length(path.Points[1] - path.Points[0]) < 1e-4f)
            path.Points.erase(path.Points.begin());   // already standing there
        return true;
    }

    bool NavGrid::LineOfSight(const PhysicsWorld& physics, const glm::vec3& from, const glm::vec3& to,
        const SightOptions& o, glm::vec3* hitPoint) const {
        const glm::vec3 a = from + glm::vec3(0.0f, o.EyeHeight, 0.0f), b = to + glm::vec3(0.0f, o.TargetHeight, 0.0f);
        const glm::vec3 d = b - a;
        const float len = glm::length(d);
        if (len < 1e-4f) return true;
        RayHit hit;
        if (!physics.Raycast(a, d, len, hit, entt::null, true)) return true;
        if (hitPoint) *hitPoint = hit.Point;
        return false;
    }

    NavGrid::Cover NavGrid::CoverAt(const PhysicsWorld& physics, const glm::vec3& at, const glm::vec3& from) const {
        glm::vec3 dir = from - at;
        dir.y = 0.0f;
        const float dist = glm::length(dir);
        if (dist < 1e-3f) return Cover::None;
        dir /= dist;
        const glm::vec3 side(-dir.z, 0.0f, dir.x);
        const float reach = std::min(dist, m_Settings.CellSize * 1.1f);   // obstacles right next to the defender
        auto blocked = [&](float height) {
            int hits = 0;
            for (float s : { -1.0f, 0.0f, 1.0f }) {                       // across the body, so a post doesn't count as a wall
                const glm::vec3 o = at + side * (s * m_Settings.AgentRadius) + glm::vec3(0.0f, height, 0.0f);
                RayHit hit;
                if (physics.Raycast(o, dir, reach, hit, entt::null, true)) hits++;
            }
            return hits >= 2;
        };
        if (!blocked(CoverLow)) return Cover::None;
        return blocked(CoverHigh) ? Cover::Full : Cover::Half;
    }

    void NavGrid::VisibleCells(const PhysicsWorld& physics, const glm::vec3& from, float range, const SightOptions& o,
        std::vector<int>& out) const {
        out.clear();
        if (!m_Baked) return;
        const float cs = m_Settings.CellSize;
        const int cx = (int)std::floor((from.x - m_Origin.x) / cs), cz = (int)std::floor((from.z - m_Origin.y) / cs);
        const int r = (int)std::ceil(range / cs);
        for (int iz = std::max(cz - r, 0); iz <= std::min(cz + r, m_D - 1); iz++)
            for (int ix = std::max(cx - r, 0); ix <= std::min(cx + r, m_W - 1); ix++) {
                const int column = iz * m_W + ix;
                for (int i = m_ColumnStart[column]; i < m_ColumnStart[column + 1]; i++) {
                    const glm::vec3 p = m_Cells[i].Pos;
                    const float dx = p.x - from.x, dz = p.z - from.z;
                    if (dx * dx + dz * dz > range * range) continue;
                    if (LineOfSight(physics, from, p, o)) out.push_back(i);
                }
            }
    }

    void NavGrid::Reachable(int start, float budget, float diagonal, std::vector<NavReach>& out) const {
        out.clear();
        if (!m_Baked || start < 0 || start >= (int)m_Cells.size()) return;
        const float diag = std::clamp(diagonal, 1.0f, 2.0f);
        Search& s = g_Search;
        s.Begin(m_Cells.size());
        std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
        s.Visit(start, 0.0f, -1);
        open.push({ 0.0f, start });
        while (!open.empty()) {
            const Open top = open.top();
            open.pop();
            const int c = top.Cell;
            if (s.Closed[c] == s.Gen) continue;
            s.Closed[c] = s.Gen;
            out.push_back({ c, s.G[c], s.Parent[c] });
            for (int d = 0; d < 8; d++) {
                const int n = m_Cells[c].Link[d];
                if (n < 0 || s.Closed[n] == s.Gen || m_Blocked[n]) continue;
                const float g = s.G[c] + (d < 4 ? 1.0f : diag);
                if (g > budget + 1e-4f || (s.Visited(n) && g >= s.G[n])) continue;
                s.Visit(n, g, c);
                open.push({ g, n });
            }
        }
    }

} // namespace Engine
