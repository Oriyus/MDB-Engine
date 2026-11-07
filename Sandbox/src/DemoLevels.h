#pragma once
#include <memory>

namespace Engine {
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
};

// Scripting playground: a ball you roll (player.lua), coins (coin.lua), a pad that glows while
// something is on it (pad.lua) and a spawner dropping balls (spawner.lua + ball.lua)
void BuildScriptDemo(Engine::Scene& scene, const LevelAssets& assets, Engine::PrefabLibrary& prefabs);

// A small game: collect every coin against the clock, then reach the goal on the platform.
// Lava sends you back to the start, the sweeper knocks you off, the green pad bounces you up.
// Scripts: game.lua (rules + UI), follow.lua (camera), player.lua, coin.lua, goal.lua,
// hazard.lua, bouncer.lua
void BuildCoinRush(Engine::Scene& scene, const LevelAssets& assets, Engine::PrefabLibrary& prefabs);
