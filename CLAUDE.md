# MDB Engine

A from-scratch 3D game engine and editor for Windows. C++20, OpenGL 4.5 (core, DSA style), built with CMake.
The engine is a static library (`Engine`) and the app that uses it is `Sandbox`.

## Build

Visual Studio 2026 generator. Dependencies download through CMake FetchContent.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -Wno-dev
cmake --build build --config Debug
.\build\bin\Debug\Sandbox.exe
```

- After adding or deleting a source file, re-run the configure line first. `Engine/CMakeLists.txt` collects sources with `file(GLOB_RECURSE ...)`.
- Edits to existing files only need the build line.
- Shaders are read from disk at runtime. **F5** in the app reloads them, so no rebuild is needed.
- The absolute asset path is the `ASSET_DIR` compile definition, set in `Sandbox/CMakeLists.txt`.
- Unattended test runs: the environment variables `MDB_DEMO=1` (build the script demo), `MDB_AUTOPLAY=1` (press Play), `MDB_TEST_SCRIPT=Scripts/x.lua` (add a "Test" entity running it), `MDB_SCENE=<file>` / `MDB_SAVE_SCENE=<file>` (load first / save last, in `Assets/Scenes`) and `MDB_QUIT_AFTER=<seconds>`. Redirect stdout to read the log.

## Pitfalls (these cost time before)

- **Never create files through Visual Studio's Solution Explorer.** It appends quoted file names to the first `add_library(...)` in `Engine/CMakeLists.txt`, which compiles engine files into the wrong target (errors like "Cannot open include file" inside `imgui.vcxproj`). Create files in File Explorer or the editor, then re-run configure. If the error appears, remove the stray quoted names from `Engine/CMakeLists.txt`.
- `imgui.h` must be included **before** `ImGuizmo.h`.
- ImGui is the **docking** branch (`v1.91.0-docking`). ImGuizmo is vendored by hand in `vendor/imguizmo/` (two files). It is not fetched.
- glad and stb are vendored in `vendor/glad` (own CMakeLists) and `vendor/stb` (`stb_image.h`, implementation defined in `Texture.cpp`).
- Every sampler uniform needs its own texture unit, even when unused. Two sampler types on one unit is a GL error.
- Delete `imgui.ini` to reset the editor layout. It is git-ignored.
- `*.obj` is git-ignored for compiled objects, and `!/Assets/**/*.obj` keeps model files.
- ImGuizmo's Scale gizmo is always local space. That is by design, not a bug.
- `ScriptEngine.cpp` (sol2) needs `/bigobj`, set per file in `Engine/CMakeLists.txt`. Lua 5.4 has no CMake, so it is built there as the `lua` target.
- In sol2, `table.get_or<T>(key, def)` is ambiguous when `def` is already a `T` rvalue. Use `get<sol::optional<T>>(key).value_or(def)`.
- GLFW key state is polled once per frame. Simulated key taps (down and up in one go, like WScript `SendKeys`) are never seen, so hold the key across frames.

## Layout

```
Engine/src/
  Core/       Application, Window, Input, Log, ImGuiLayer, EntryPoint.h (owns main)
  Renderer/   Shader, Buffer, VertexArray, Texture, Camera, Mesh, Model, Material, Renderable,
              Framebuffer, ShadowMap (PointShadowArray, DirectionalShadowMap), Environment (IBL),
              PostProcess, SSAO, Bloom, RenderStats
  Scene/      Scene, Entity, Components, SceneSerializer
  Physics/    PhysicsWorld (Jolt): bodies, contact events, raycasts
  Scripting/  ScriptEngine (Lua 5.4 + sol2). The full script API is documented in ScriptEngine.h
Sandbox/src/main.cpp   the editor app (SandboxApp)
Assets/{Shaders,Models,Scenes,Environments,Scripts}
vendor/{glad,stb,imguizmo}
```

Dependencies: GLFW, glad, GLM, spdlog, EnTT, Dear ImGui (docking), ImGuizmo, assimp, nlohmann/json, stb_image, Jolt Physics, Lua 5.4, sol2.

## Architecture

- **Application** runs the loop and calls the virtual hooks `OnStart`, `OnUpdate(dt)`, `OnImGui`, `OnShutdown`. The game defines `Engine::CreateApplication()`.
- **ECS (EnTT).** `Entity` is a thin handle (`Add<T>`, `Get<T>`, `Has<T>`, `Remove<T>`). Every entity gets `TagComponent` and `TransformComponent`.
  Components: `MeshRenderer`, `PointLight`, `DirectionalLight`, `Spin`, `Orbit`, `RigidBody`, `Script`.
  Transforms store position, Euler rotation in radians, and scale.
- **Scene** holds the systems. `OnUpdate` runs Spin and Orbit. `RenderShadows` and `Render` draw the scene.
- **Adding a component type means updating four places:** `Components.h`, `EntitySnapshot` plus `CaptureEntity`/`CreateFromSnapshot` in `Scene` (play mode, `DuplicateEntity`, script clone), and `SceneSerializer` Save and Load (the live scene is filled through the snapshot functions). Add an Inspector section in `Sandbox/src/main.cpp` too.
- **Scripting.** `ScriptComponent{ Path }` points at a Lua file in `Assets/Scripts`. Scripts run only during Play. Each entity gets its own Lua environment, and the `shared` table is common to all scripts. Each frame runs `Scene::OnUpdate`, then `ScriptEngine::Update` (`on_update`), then `PhysicsWorld::Step`, then `ScriptEngine::DispatchCollisions` (`on_collision_begin/end`, `on_trigger_enter/exit`). On Play, `PhysicsWorld::Sync` runs before `ScriptEngine::Start`, so `on_create` can already use `self.body`. Script destroys and script swaps are deferred until nothing is iterating. A script that errors is disabled and its error is shown in the Tools panel.
- **Physics events.** Jolt's contact listener only records contacts on worker threads. On the main thread they are counted per body pair, so each pair gets exactly one begin and one end. Removing a body closes its pairs, so scripts get an end or exit for destroyed entities too. Triggers are created as kinematic sensors, because a static sensor drops its contacts when the body inside falls asleep. `add_force` applies in every fixed step of the frame (like gravity), and `add_impulse` applies once. Changing a static or dynamic body's transform from outside teleports the body.
- **Renderables** (`Mesh`, `Model`) share the `Renderable` interface. This includes `GetLocalBounds` and the submesh/material hooks.
- **Scene files** (`Assets/Scenes/scene.json`) store resource **names** ("cube", "plane", "sphere", "model", "checker"), mapped back to live resources by `MakeResources()` in the Sandbox. A `settings` block (`SceneSettings`) holds the environment map name, sky, ambient, post, AO, bloom, shadows, gravity and the editor camera. Every section and field is optional, so older files load fine. The Sandbox fills it with `GatherSettings` and `ApplySettings`. Load is queued and applied at the start of `OnUpdate`. `demo.json` is the saved script demo.

## Render pipeline (per frame)

1. Shadow passes: up to 4 point lights (cubemap array, 6 faces each) and one sun (ortho 2D map).
2. Scene into an **HDR framebuffer** (RGBA16F color, an `R32I` entity-ID attachment used for click picking, and a depth texture).
   The shader is **PBR metallic-roughness** (Cook-Torrance GGX), with normal, metal/rough and emissive maps.
   Ambient comes from **IBL**: irradiance and prefiltered cubemaps baked from an HDR panorama, or a procedural sky if none is loaded.
   The skybox is drawn after the scene.
3. SSAO from depth, then a blur.
4. Bloom: a down/up chain, with a soft threshold, a Karis average and a per-pixel brightness ceiling to stop fireflies.
5. Post pass: AO, plus bloom, exposure, tone map (ACES default), gamma.
6. FXAA pass. The result is shown in the ImGui Viewport panel.

Color spaces: lighting is done in linear space. Color pickers and albedo colors are sRGB-style and converted in the shader. Albedo and emissive textures are sRGB textures. Normal and metal/rough textures are linear.

## Editor

- Dockable panels: Viewport, Hierarchy, Inspector, Stats, Tools.
- **Maya-style controls.** Alt+LMB orbit, Alt+MMB pan, Alt+RMB or wheel dolly. **Q** select, **W/E/R** move/rotate/scale gizmo. **F** frame selected. **N** spawn a cube. **Delete** deletes the selection. **F5** reloads shaders, and scripts too while playing. **F1** hides the side panels. There is no WASD fly camera (it was removed on purpose).
- **Play mode** (Ctrl+P, or the Tools panel buttons). Play snapshots the scene and runs Spin, Orbit and physics. Pause and Step are available. Stop restores the snapshot, and Escape stops Play first and quits only in Edit mode.
  Edit mode is frozen except that orbiting lights sit at their orbit position.
- Tools panel > Scene > **Build script demo** replaces the scene with a small level. It uses `player.lua`, `coin.lua`, `pad.lua`, `spawner.lua` and `ball.lua`.
- Physics runs only during Play, at a fixed 60 Hz with interpolated transforms. Body types are Static, Dynamic and Kinematic, shapes are Box and Sphere, and any body can be a Trigger (drawn in magenta). **T** throws a ball during Play. Collider outlines are drawn with ImGui.

## Conventions

- DSA OpenGL calls (`glCreate*`, `glNamed*`). Wrapper classes own GL objects (RAII) and are non-copyable.
- Shader uniforms go through `Shader::Set*`, which uses `glProgramUniform*` and caches locations.
- Logging macros: `ENGINE_INFO/WARN/ERROR` (engine side) and `LOG_INFO/WARN/ERROR` (game side).
- State changes that invalidate the frame (environment bake, play start/stop) are queued as commands and applied at the start of `OnUpdate`, never in the middle of building the ImGui frame.
- Pending fullscreen passes use a vertex-less triangle (`tonemap.vert`), with an empty VAO bound.

## Known limitations and ideas

- Physics colliders are boxes and spheres only. Convex-hull and mesh colliders are not done. Triggers detect dynamic bodies only (not kinematic or static ones).
- Changing a RigidBody in the Inspector during Play applies on the next Play. Scripts rebuild a body with `add_body`.
- Only the first sun and up to 4 point lights are used (`MaxLights`, `MAX_POINT_LIGHTS`).
- SSAO is a post-multiply (darkens direct light too), and its blur is not depth-aware.
- Models are imported with `PreTransformVertices`, so there is no node hierarchy or skeletal animation.
- No transparency or blending pass. Materials with alpha render opaque.
- The sun shadow area is fixed around the origin (no cascades).
- Candidate next steps: a runtime game camera, prefabs, on-screen text for scripts, audio, mesh/convex colliders, skeletal animation, transparency, batching/instancing, a Vulkan backend.
