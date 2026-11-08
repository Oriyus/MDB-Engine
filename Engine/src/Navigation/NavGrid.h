#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Engine {

    class PhysicsWorld;

    struct NavSettings {
        float CellSize = 1.0f;          // metres per cell (square, in X and Z)
        float AgentRadius = 0.3f;       // a cell is walkable only if no wall is this close to its centre
        float AgentHeight = 1.8f;       // free space needed above a cell (less under a bridge = not walkable there)
        float MaxStep = 0.4f;           // the largest height difference between two linked cells (stairs, ramps)
        float MaxSlope = 40.0f;         // degrees: steeper surfaces are not walkable
        bool AutoBounds = true;         // bake over the bounds of every static body
        glm::vec3 BoundsMin{ -50.0f }, BoundsMax{ 50.0f };   // used when AutoBounds is off
    };

    struct NavPathOptions {
        float Diagonal = 1.41421356f;   // the cost of a diagonal step (1 orthogonal step costs 1)
        bool Grid = false;              // combat: keep cell centres, start and end on cells, no smoothing
        bool Closest = true;            // goal unreachable: go to the reachable cell nearest to it
        float MaxCost = -1.0f;          // > 0: stop the path where its cost would go over this
    };

    struct NavPath {
        std::vector<glm::vec3> Points;  // where to walk, start first
        std::vector<int> Cells;         // the cells the path crosses (grid order, before smoothing)
        std::vector<float> Costs;       // Grid paths: cost up to each point (Points[0] = 0)
        float Cost = 0.0f;              // total cost (in steps)
        bool Complete = true;           // false: the goal could not be reached, this ends nearest to it
    };

    struct NavReach {
        int Cell = -1;
        float Cost = 0.0f;
        int Previous = -1;              // the cell it is reached from (-1 for the start)
    };

    // A layered walkability grid, baked from the static physics bodies: columns of cell size in X/Z,
    // each holding every walkable height found there (ground under a bridge and the bridge deck are two
    // cells of one column). Cells link to their 8 neighbours when the height difference is a step and
    // no wall stands between them; diagonals also need both orthogonal neighbours (no corner cutting).
    //
    // Exploration paths are A* on these cells, then string-pulled into straight lines wherever the grid
    // allows (so movement looks free). Grid paths keep the cell centres and count steps (combat).
    // Blocked cells (units standing there, closed doors) are set by the game and never baked.
    class NavGrid {
    public:
        bool Bake(const PhysicsWorld& physics, const NavSettings& settings);
        void Clear();
        bool IsBaked() const { return m_Baked; }
        const NavSettings& Settings() const { return m_Settings; }

        int CellCount() const { return (int)m_Cells.size(); }
        glm::vec3 CellPosition(int cell) const;                 // the centre of the cell's walkable surface
        int CellLayer(int cell) const;                          // 0 = lowest cell of its column
        int Neighbor(int cell, int dir) const;                  // dir 0..7 (+X, -X, +Z, -Z, then diagonals), -1 = none

        // The cell standing under `p` (p at most MaxStep below the surface, at most AgentHeight above), -1 if none
        int CellAt(const glm::vec3& p) const;
        // The nearest walkable cell within maxDistance (in X/Z), ignoring blocked ones if asked; -1 if none
        int Nearest(const glm::vec3& p, float maxDistance, bool skipBlocked) const;

        void SetBlocked(int cell, bool blocked);
        bool IsBlocked(int cell) const;
        void ClearBlocked();

        bool FindPath(const glm::vec3& from, const glm::vec3& to, const NavPathOptions& options, NavPath& path) const;
        // Every cell reachable from `start` within `budget` (cost), the start included, cheapest first
        void Reachable(int start, float budget, float diagonal, std::vector<NavReach>& out) const;
        // Can an agent walk straight from a to b (both on the grid)? Blocked cells stop it.
        bool LineClear(const glm::vec3& a, const glm::vec3& b) const;

        float LastBakeMs() const { return m_BakeMs; }

    private:
        struct Cell {
            glm::vec3 Pos{ 0.0f };
            int Column = 0;
            int Link[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        };

        bool ColumnOf(float x, float z, int& ix, int& iz) const;
        int CellInColumn(int ix, int iz, float y) const;
        int StartCell(const glm::vec3& p) const;
        bool LineClearFrom(int startCell, const glm::vec3& a, const glm::vec3& b, int goalCell) const;

        NavSettings m_Settings;
        bool m_Baked = false;
        float m_BakeMs = 0.0f;
        glm::vec2 m_Origin{ 0.0f };          // X/Z corner of column (0, 0)
        int m_W = 0, m_D = 0;                // columns along X and Z
        std::vector<int> m_ColumnStart;      // m_W * m_D + 1 prefix sums into m_Cells (each column bottom to top)
        std::vector<Cell> m_Cells;
        std::vector<uint8_t> m_Blocked;
    };

} // namespace Engine
