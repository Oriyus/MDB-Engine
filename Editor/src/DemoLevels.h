#pragma once
#include <functional>
#include <memory>

namespace Engine {
    struct TerrainLayer;
    struct TerrainSurfaceMaps;
    class Scene;
    class Renderable;
    class Texture2D;
    class Model;
    class PrefabLibrary;
}

// Code-built example levels. Each one replaces the scene's contents. Coins (and the script demo's
// dropped balls) are prefabs: Prefabs/coin.json and Prefabs/drop_ball.json, written on first use
// if missing, so edits to those files are kept. Once built the levels are ordinary
// scenes: save them (Tools > Scene) and edit them like any other level.
struct LevelAssets {
    std::shared_ptr<Engine::Renderable> Cube;    // unit cube
    std::shared_ptr<Engine::Renderable> Plane;   // 12 x 12 floor
    std::shared_ptr<Engine::Renderable> Sphere;  // diameter 1
    std::shared_ptr<Engine::Texture2D>  Checker;
    std::shared_ptr<Engine::Model>      Fox;     // Models/Fox.glb (animated); null if missing
    std::shared_ptr<Engine::Texture2D>  Fence;   // Textures/fence.png (cutout); null if missing
    std::shared_ptr<Engine::Texture2D>  Window;  // Textures/window.png (stained glass)
    std::shared_ptr<Engine::Renderable> Capsule; // 1 wide, 2 tall (placeholder characters)
    std::shared_ptr<Engine::Renderable> Cone;    // 1 wide base, 1 tall
    std::shared_ptr<Engine::Texture2D>  TerrainLayers[4];   // Textures/Terrain/{grass,earth,rock,sand}.jpg (may be null)
    // A terrain layer's packed normal / roughness / height maps (ResourceLibrary::LoadTerrainSurface)
    std::function<std::shared_ptr<const Engine::TerrainSurfaceMaps>(const Engine::TerrainLayer&)> TerrainSurface;
};

// Scripting playground: a ball you roll (player.lua), coins (coin.lua), a pad that glows while
// something is on it (pad.lua) and a spawner dropping balls (spawner.lua + ball.lua)
void BuildScriptDemo(Engine::Scene& scene, const LevelAssets& assets, Engine::PrefabLibrary& prefabs);

// A small game: collect every coin against the clock, then reach the goal on the platform.
// Lava sends you back to the start, the sweeper knocks you off, the green pad bounces you up.
// Scripts: game.lua (rules + UI), follow.lua (camera), player.lua, coin.lua, goal.lua,
// hazard.lua, bouncer.lua
void BuildCoinRush(Engine::Scene& scene, const LevelAssets& assets, Engine::PrefabLibrary& prefabs);

// Navigation test for a turn-based game: free movement while exploring, grid movement in combat, on
// one layered nav grid. Terraces joined by a bridge (walkable above and below), a ramp, steps up to a
// dais, a thin fence on a cell boundary and crates. The fox hero and three enemy blocks have no body:
// tactics.lua moves them. Scripts: tactics.lua (modes, turns, overlay tiles), tactics_camera.lua
void BuildTacticsTest(Engine::Scene& scene, const LevelAssets& assets, Engine::PrefabLibrary& prefabs);

// The second tactics level: a cellar reached by the yard's glowing pad (and back). Both levels have a
// chest (chest.lua) whose gold goes into the saved `game` table. exit.lua changes level.
void BuildTacticsCellar(Engine::Scene& scene, const LevelAssets& assets, Engine::PrefabLibrary& prefabs);

// Heightmap terrain: 120 x 120 m of generated hills (Terrains/terrain.terrain) painted by their shape (grass,
// rock on slopes, sand by the water), grass growing on the grass layer, water, and balls rolling down the slopes.
// Select the Terrain for its sculpt and paint brushes.
void BuildTerrainDemo(Engine::Scene& scene, const LevelAssets& assets, Engine::PrefabLibrary& prefabs);
