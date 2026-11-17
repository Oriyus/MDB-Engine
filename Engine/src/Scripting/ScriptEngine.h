#pragma once
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ImFont;

namespace Engine {

    class Scene;
    class PhysicsWorld;
    class AudioEngine;
    class PrefabLibrary;
    class NavGrid;
    class FogOfWar;
    class InputMap;
    struct ResourceRegistry;

    // What saves.* and level.* reach (GameSession implements it)
    class ScriptHost {
    public:
        virtual ~ScriptHost() = default;
        virtual bool SaveGame(const std::string& slot, const std::string& name, std::string& error) = 0;
        virtual bool RequestLoad(const std::string& slot) = 0;       // false: no such save
        virtual bool DeleteSave(const std::string& slot) = 0;
        virtual nlohmann::json ListSaves() const = 0;               // newest first
        virtual void RequestLevel(const std::string& scene, const std::string& entry, const std::vector<uint64_t>& carry) = 0;
        virtual std::string CurrentLevel() const = 0;
        virtual std::string LevelEntry() const = 0;
        virtual bool Arrived() const = 0;          // this level was entered by level.change (not a load or a new game)
        virtual float PlayTime() const = 0;
    };

    // One ui.* call: drawn by the app over the game view for the frame it was made in, in call
    // order (later items on top). Positions and sizes are 0..1 of the view's width / height.
    struct ScriptUIItem {
        // Border: a rectangle outline. ClipPush / ClipPop: later items are cut to X, Y, W, H (nested).
        enum class Type { Text, Rect, Image, Button, Border, ClipPush, ClipPop };
        Type Kind = Type::Text;
        std::string Text;                   // the text, a button's label, or an image path ("Textures/x.png")
        float X = 0.0f, Y = 0.0f;           // text: anchor (see Align); others: top left
        float W = 0.0f, H = 0.0f;           // rect / image / button size (an image with H = 0 keeps its aspect)
        float Size = 24.0f;                 // text size in pixels at a 720 px tall view (scales with it)
        glm::vec3 Color{ 1.0f };            // text colour, fill, image tint, button face
        glm::vec3 TextColor{ 1.0f };        // a button's label
        float Alpha = 1.0f;
        float Rounding = 0.0f;              // corner radius in pixels at 720p
        int Align = 0;                      // text: 0 left, 1 center, 2 right (of X)
        bool Hovered = false, Pressed = false;   // buttons, for drawing
        bool Regular = false;               // text: the regular font (default: bold)
        bool Shadow = true;                 // text: a soft drop shadow
        float Thickness = 1.0f;             // border width in pixels at 720p
        float Slice = 0.0f;                 // image: 9-slice frame, this many texture pixels kept unstretched on each side
    };
    using ScriptText = ScriptUIItem;        // the older name

    // Runs the Lua scripts of every entity with a ScriptComponent while the scene plays.
    // Each entity gets its own copy of the script's globals, so two entities running the same
    // file don't share state. A script that errors is logged and switched off; the rest keep running.
    // Each Start() builds a fresh Lua state and re-reads the files (so does Reload()).
    //
    // ---- Callbacks (all optional)
    //   on_create(self)                         once, when Play starts or the entity appears
    //   on_update(self, dt)                     every simulated frame
    //   on_destroy(self)                        on Stop / Reload, or when the entity is destroyed by a script
    //   on_collision_begin(self, other, hit)    started touching; hit.point, hit.normal (points from other to self)
    //   on_collision_end(self, other)           stopped touching (other may already be destroyed: other:valid())
    //   on_trigger_enter(self, other)           one of the two is a trigger (sensor) body
    //   on_trigger_exit(self, other)
    //   on_anim_event(self, name, clip)         an Animator event fired (see e.animator), or "end" of a non-looping clip
    //   on_save(self) -> table                  a save game (or leaving the level) stores what it returns
    //   on_load(self, data)                     right after on_create, when the level is restored: data = that table
    //   on_event(self, name, data)              someone called events.emit(name, data) (delivered after the frame's on_update)
    //
    // ---- Save games and levels (Runtime/GameSession). Entities are saved by the engine (everything that
    // differs from the scene file); a script saves its own variables through on_save / on_load. Saved
    // tables hold nil, booleans, numbers, strings, vec3, entities (restored by uid) and nested tables.
    //   game                                 a table saved with the game and kept across level changes (party,
    //                                        quests, gold). `shared` is per level (saved with the level).
    //   saves.save(slot [, name]) -> true or false, error      at once; slot: letters, digits, _ and -
    //   saves.load(slot) -> true or false     the game loads at the start of the next frame
    //   saves.list() -> { { slot, name, level, time = "2026-11-08 14:02", play_time }... }   newest first
    //   saves.exists(slot), saves.delete(slot)
    //   level.change(scene [, entry [, { carry = { entities } }]])   next frame: this level is kept as it is
    //       (coming back restores it), the new one loads (as left, if visited). Carried entities (and their
    //       children, with their script data) move along. entry: a name for scripts (level.entry()).
    //   level.current() -> scene file, level.entry() -> entry or nil, level.play_time() -> seconds
    //   level.arrived() -> true if this level was just entered by level.change (also after a restart of it):
    //       place the party at the entry then; after a load it stands where it was saved
    //   scene.is_loading() -> true during on_create / on_load of a restored level (a save, or a level visited
    //       before): don't spawn level content then, it was saved
    //   e.uid -> stable id (kept by saves), scene.find_uid(uid), e.transient (true: never saved, e.g. markers)
    // ---- Events, data files, libraries
    //   events.emit(name [, data])           every script with on_event(self, name, data) hears it (this frame)
    //   data.load(path) -> table or nil, error   any JSON file under Assets, as a Lua table
    //   Scripts/Lib/*.lua run once (name order) in the shared globals when scripts start: shared code, like
    //   Lib/dialogue.lua (dialogue.open). Scripts can add to those tables (dialogue.helpers.give = ...).
    //
    // ---- Entity (self, other, results of scene.find / spawn / clone)
    //   e.name, e.position, e.rotation (radians), e.scale     read/write, LOCAL (relative to the parent);
    //                                                         vec3 values are copies
    //   e.world_position                      read/write, world space
    //   e.parent (entity or nil, settable: keeps the world placement), e:children()
    //   e.forward, e.right, e.up                              world-space directions (read-only)
    //   e:look_at(target [, up]) and e.forward... work in world space
    //   e.id, e:valid(), e:destroy(), e:clone()
    //   e:flash([color = white [, seconds = 0.25 [, strength = 3]]])   a hit flash on the entity and every mesh below it
    //   e.visible   shown or hidden (runtime, not saved): the entity and every mesh below it; hidden ones cast no shadow
    //   e:has(kind), e:remove(kind)     kind = "mesh" "body" "character" "collider" "light" "sun" "script" "spin" "orbit" "camera" "audio" "animator" "particles"
    //   e.mesh   -> nil or { color, emissive, metallic, roughness (per-object overrides of the material's values: reading gives
    //               the override, else the first part's material; clear_overrides() removes them), geometry ("cube" "sphere" ...),
    //               opacity (0..1; below 1 makes an opaque mesh "blend"), alpha_mode ("opaque" "cutout" "blend"),
    //               alpha_cutoff, cast_shadows, static (merged when Play starts; read-only in effect during Play),
    //               wind (sway in the scene's wind: 0 still, 1 a tree), double_sided (back faces drawn too),
    //               specular, unlit (read only: the material's),
    //               lods (levels of detail: a list of { geometry, distance }, nearest first; reading gives a copy, assign a
    //               whole list to change them), cull_distance (metres; 0 = drawn at any distance), lod (read only: the level
    //               drawn last frame, 0 = geometry, #lods + 1 = beyond cull_distance),
    //               material ("Materials/x.mat", "" = none; setting it gives every part that file), materials (one path per part,
    //               "" = the model's own or the Default; a copy, assign a whole list); a missing file is an error }
    //   e.light  -> nil or { color, intensity, shadows, range (metres, 0 = automatic), reach (read only: the range in use), radius (its size, m: softer shadows, wider highlights), contact_shadows (bool), shape ("sphere" / "tube" / "rect": area lights;
    //               a tube runs along local X, a rect is local X by Y shining along -Z), width, height (m, before scale),
    //               spot_angle (the cone's full angle in degrees, along local -Z; 0 = a point light), spot_blend (0..1: the soft edge), cookie (an image under Assets the light shines
    //               through, "Textures/x.png"; "" = none; spots project it along the cone, points wrap it round as a 2:1
    //               panorama centred on local -Z; its top follows local +Y), cookie_columns / cookie_rows (a flipbook grid,
    //               left to right, top row first), cookie_fps (0 = hold cookie_frame), cookie_frame, profile (an IES file,
    //               "Lights/x.ies": brightness by direction, 0 degrees along local -Z; "" = none) }
    //   e.body   -> nil or { velocity, angular_velocity, type, trigger, lock_rotation, mass,
    //                         add_force(v) (newtons, lasts this frame: call each frame), add_impulse(v) }
    //   e.character -> nil or a character controller (CharacterControllerComponent: walks, steps, slopes, platforms) {
    //                 move(v)   the wanted sideways velocity, m/s, world (y ignored); kept until changed: set it every frame
    //                 wanted    (read) that velocity
    //                 jump(speed) -> bool   leave the ground upwards at speed m/s; false (nothing) unless grounded
    //                 velocity  (read) what it moved with last step; (write) a launch / knock-up: an upward y lasts
    //                           until it lands, the sideways part becomes the wanted move
    //                 grounded, on_steep_ground (sliding down ground steeper than max_slope), ground_normal,
    //                 ground_velocity (a moving platform's), ground_point, ground_entity (nil in the air)
    //                 set_size(height [, radius]) -> bool   crouch / stand: the feet stay; false if there is no room
    //                 radius, height, center (read); max_slope (degrees), step_height, stick_down, mass, push_strength
    //                 (newtons), gravity_scale (read / write) }
    //                 Teleport by setting e.position. Gravity is applied for you; physics never turns it.
    //   e:add_character{ radius=0.4, height=1.8, center=vec3 (the capsule's centre from the origin; 0 = origin in the middle),
    //                    max_slope=50, step_height=0.35, stick_down=0.5, mass=70, push_strength=300, gravity_scale=1 }
    //                    a rigid body on the same entity is ignored (the character is its body)
    //   e:add_mesh{ geometry="cube"|"sphere"|"plane"|"grid"|"capsule"|"cylinder"|"cone"|"model:Models/x.glb", color=vec3 (and
    //               metallic, roughness, emissive: overrides), fit=metres (models: scaled to that size, standing on the origin),
    //               opacity, alpha_mode, alpha_cutoff, cast_shadows, static, wind, double_sided (texture, unlit and
    //               specular are material values: an error here),
    //               lods = { { geometry = "model:Models/x_lod1.glb", distance = 25 }, ... }, cull_distance,
    //               material = "Materials/x.mat" (every part) or materials = { "Materials/a.mat", "", ... } (per part) }
    //   material.exists(path) -> bool        a material file (Assets/Materials/*.mat; Renderer/MaterialAsset.h names its keys)
    //   material.get(path) -> table           every value after its parent's (colours as vec3, maps as paths, parent, params of a
    //                                         shader graph material, error: "" or why it draws magenta)
    //   material.set_param(path, name, value) a shader graph param (number, list, vec3 colour or texture path), for
    //                                         everything using the material, until Play stops
    //   material.set(path, key, value)        changes it for every part and terrain layer using it, until Play stops
    //                                         (never saved); an unknown key or a wrong type is an error
    //   e:add_body{ type="dynamic"|"static"|"kinematic", shape="box"|"sphere"|"capsule"|"convex"|"mesh"|"none", size=number|vec3,
    //               center=vec3, mass, friction, bounciness, trigger=bool, velocity=vec3,
    //               lock_rotation=bool (dynamic: physics never turns it; characters) }
    //   e:add_collider{ shape="box"|"sphere"|"capsule"|"convex"|"mesh", size, center }   an extra shape for the
    //               rigid body above this entity (compound collider). Capsule size = vec3(diameter, height, 0)
    //   e:add_light{ color=vec3, intensity, shadows, range, radius, contact_shadows, shape, width, height, spot_angle, spot_blend, cookie, cookie_columns, cookie_rows,
    //                 cookie_fps, cookie_frame, profile }
    //   e:add_script("Scripts/x.lua")        starts on the next frame
    //   e.camera -> nil or { fov, primary, small_objects, grass_distance }
    //                e:add_camera{ fov=60, near, far, primary=true, small_objects=0, grass_distance=1 }
    //                small_objects: parts smaller on screen than this share of the view height are not drawn (0 = off,
    //                0.01 = 1%); grass_distance multiplies every grass field's draw distance (0 = no grass)
    //   e:look_at(target [, up])               turn local -Z (forward) towards a point
    //   e.audio -> nil or { clip, volume, pitch, loop, bus, playing, play(), stop() }
    //   e:add_audio(clip [, { volume, pitch, loop=true, spatial=true, play=true, min_distance, max_distance, bus="sfx" }])
    //   e.particles -> nil or { emitting, rate, emission, gravity_scale, start_color, end_color (vec3), count,
    //                           burst([n]), clear(), preset(name) }   presets: Fire Smoke Sparks Magic Dust Embers
    //   e:add_particles("Fire") or e:add_particles{ preset, rate, burst, emitting, additive, texture = "texture:Textures/x.png" }
    //   e.animator -> nil or { clip (set = cross-fade), speed, loop, playing, time, blend_time, root_motion, root_bone,
    //                          play(name [, blend_seconds]), stop(), duration([name]), clips(), bones(),
    //                          add_event(clip, time, name), clear_events([clip]), events(),
    //                          set_layer(n, { clip, mask = bone name, weight, speed, loop, additive }), layer(n),
    //                          layer_weight(n, weight [, fade_seconds]), remove_layer(n), layer_count(),
    //                          set_ik(n, { type = "look"|"limb", bone, target = vec3 or entity, offset, weight,
    //                                      max_angle, chain, pole }), ik_weight(n, w), remove_ik(n), ik_count(),
    //                          ik(n) -> { type, bone, target = world point, entity, offset, weight, max_angle, chain, pole },
    //                          set_feet{ enabled, bones = { names }, pelvis, weight, ray_up, ray_down, max_pelvis_drop,
    //                                    align_slope, max_slope, smoothing }  (foot placement: feet on uneven ground),
    //                          feet() -> those keys + lift / ground / normal per foot, pelvis_drop, fade,
    //                          bone_position(name) -> world vec3 or nil (after layers and IK),
    //                          bone_forward(name) -> the world direction it faces (at rest: the entity's +Z) }
    //     Layers play another clip over the base one on a bone and everything below it (mask "" = all).
    //     An additive layer adds its clip's motion (relative to its first frame) instead of replacing the pose.
    //     root_motion: the root bone's sideways travel in the base clip moves the entity (no body, or kinematic).
    //     IK "look" turns bone (a head) and chain-1 parents towards the target, at most max_angle from the
    //     way the entity faces (+Z); "limb" bends bone's parent and grandparent so bone reaches the target.
    //     Events fire on_anim_event(self, name, clip) when playback crosses their time; a non-looping clip
    //     also fires "end". An event named "sound:Sounds/x.wav" plays that sound at the entity instead.
    //   Animation graph (Animation/AnimGraph.h, AnimGraph Editor): e.animator.graph = "AnimGraphs/x.json" ("" = clips again),
    //     set(name, number | bool), get(name), trigger(name), reset_trigger(name), state([layer=1]) -> name,
    //     state_time([layer]) -> seconds in it, phase([layer]) -> 0..1, in_transition([layer]),
    //     play_state(name [, layer=1, blend=0.2]). Entering a state calls on_anim_event(self, "enter", <state name>).
    //   e:add_animator{ clip, speed=1, loop=true, blend=0.25, graph = "AnimGraphs/x.json" }   needs a Mesh Renderer with an animated model
    //
    // ---- Globals
    //   vec3(x, y, z): + - * / unary-, v:length(), v:normalize(), v:dot(w), v:cross(w)
    //   scene.find(name), scene.find_all(name), scene.spawn(name [, position]), scene.time()
    //   scene.spawn_prefab(name [, position])  an instance of Prefabs/<name>.json (nil if missing)
    //   scene.restart()                      Stop + Play on the next frame (retry a level)
    //   audio.play(clip [, { volume, pitch, bus = "sfx" }])  2D one-shot, e.g. "Sounds/coin.wav"
    //   audio.play_at(clip, position [, { volume, pitch, bus }]) 3D one-shot, quieter with distance
    //   Buses (Audio/AudioMix.h): "sfx" (default), "music", "voice", "ui", "ambience". Pause stops all but "ui".
    //   audio.set_volume(bus, 0..1), audio.volume(bus), audio.set_muted(bus, bool), audio.muted(bus)   bus may be "master";
    //     in the game these are the player's settings (saved on quit), in the editor Stop puts back the mix Play began with
    //   audio.buses() -> { "sfx", "music", ... }, audio.voices([bus]) -> sounds alive (all, or on that bus)
    //   scene.quit()                         the game closes (in the editor: Play stops)
    //   scene.render_stats() -> { draw_calls, triangles, instances, culled, shadow_skipped, particles, decals, static_batches, static_merged, foliage, lod_reduced, lod_culled,
    //       lights, lights_visible, lights_per_cluster,   (point lights: all, those the camera's view reaches, the most in one cluster)
    //       shadowed_lights, atlas_lights, atlas_smallest, atlas_largest, atlas_used }   (lights with shadows; in the shadow atlas, their squares in texels, the share of it used 0..1)
    //                                        (the last frame; a debug HUD)
    //   scene.light_binning(["gpu"|"cpu"]) -> where point lights are sorted into clusters (default gpu: a compute shader);
    //               sets it first if given, returns the one in use (cpu when compute shaders failed to build)
    //   scene.lod_scale([v])  -> the scene's level-of-detail distance scale (every switch and cull distance x v; 2 = detail
    //               twice as far): reads it, or sets it first
    //   scene.spawn_particles(pos, "Sparks" | { preset, count })   a one-shot burst entity that removes itself when done
    //   ---- ui: drawn this frame only, in call order. x, y, w, h are 0..1 of the view (top left origin).
    //   ui.text(text, x, y [, { size=24, color=vec3, alpha=1, align="left"|"center"|"right" }])
    //   ui.rect(x, y, w, h [, { color, alpha, rounding }])                 rounding in px at 720p
    //   ui.bar(x, y, w, h, fraction [, { color, back=vec3, alpha, rounding }])   e.g. health or time
    //   ui.image(path, x, y, w [, h] [, { color (tint), alpha }])          "Textures/x.png"; no h: keep aspect
    //   ui.button(label, x, y, w, h [, { color, text_color, size, alpha, rounding }]) -> true on the frame it's clicked
    //   ui.mouse() -> x, y, inside                                         pointer over the view, 0..1
    //   ---- laid-out UI (Runtime/UILayout.h): panels place their contents. Sizes are units (pixels at 720p).
    //   ui.begin_panel(id, { x=0.5, y=0.5, anchor="center", w=400, h (0: fit the content), title, close=false,
    //                        padding, color, alpha, image, slice }) -> open (false on the frame its x is clicked)
    //       anchor: which point of the panel sits at x, y (0..1 of the view): "center" "top" "bottom" "left" "right"
    //       "topleft" "topright" "bottomleft" "bottomright". Every begin_* needs its end_*.
    //   ui.end_panel(), ui.begin_row([spacing]) / ui.end_row() (side by side), ui.begin_scroll(id, height) / ui.end_scroll()
    //   ui.begin_column([width]) / ui.end_column()   a stack inside a row. w = 0 on a panel: as wide as its content
    //       (give bars a width there; text doesn't wrap)
    //   ui.label(text [, { size, color, align="left"|"center"|"right", bold, muted, wrap=true, shadow, width }])
    //       wraps to the width left; markup: {#ffcc66}coloured{/}, {b}bold{/b}, "\n" new line
    //   ui.button(label [, { w, h, fill, size, color, disabled, selected, tooltip, id }]) -> clicked (on release)
    //   ui.image(path [, { w=64, h=w, color, alpha }]), ui.bar(fraction [, { w, h=18, color, back, text }])
    //   ui.slot(id [, { icon, count, size, tooltip, drag = value, accept=true, highlight, color }])
    //       -> clicked, dropped, right_clicked, hovered.  drag: this slot's icon can be dragged; dropped is the
    //       dragged slot's value when a drag ends here (nil otherwise)
    //   ui.tabs(id, { "A", "B" }, current) -> index, ui.space([units]), ui.separator()
    //   ui.tooltip(text)                     for the widget just placed (widgets also take tooltip = ...)
    //   ui.input(id, text [, { w, h, size, hint, max = chars, password, filter = "integer"|"decimal"|"name", disabled,
    //       focus, select_all = true, keep_focus, tooltip }]) -> text, submitted, focused
    //       A one-line text field: keep the returned text and pass it back next frame. A click (or Tab / Shift+Tab, or
    //       focus turning true) gives it the keyboard; caret, selection (drag, Shift, Ctrl+A), Ctrl+arrows,
    //       Ctrl+C / X / V. Enter: submitted (and it lets go, unless keep_focus), Escape: back to the text it had when
    //       focused. Changing the text you pass in while it is focused (clearing it after Enter) is taken over.
    //   ui.typing() -> a text field has the keyboard: game keys and input actions read as up meanwhile (and keys
    //       held when it lets go stay up until released); ui.unfocus() takes the keyboard back
    //   ui.simulate_type(text), ui.simulate_key("ctrl+a" | "shift+left" | "backspace" | "enter" ...)   test hooks:
    //       typing for the next frame
    //   ui.slider(id, value [, { min = 0, max = 1, step, w, h = 24, text, color, disabled, tooltip }]) -> value, changed
    //   Gamepad / keyboard navigation (input actions ui_up / ui_down / ui_left / ui_right / ui_confirm / ui_back /
    //       ui_secondary; defaults: arrows, Enter, Escape and the pad's D-pad / left stick, A, B, X, unless input.json
    //       binds them): buttons, slots, text fields and sliders can take the focus; directions move it to the nearest
    //       one that way (its panel first), Tab / Shift+Tab in draw order, confirm presses it (a text field starts
    //       typing, a slot with drag picks its icon up and the next confirmed slot gets it as dropped), secondary
    //       right-clicks a slot, Left / Right change a slider. Back closes the focused (else the topmost) panel with
    //       close = true, else ui.back() returns true once. The ring shows after a navigation input (or when the pad
    //       was used last) and hides when the mouse moves; a panel that appears takes the focus (its widget with
    //       default_focus = true, else its first). ui.navigating() -> the ring shows, ui.nav_focused() -> the widget
    //       just placed has the focus, ui.focus_default(). Test with input.simulate_key / simulate_gamepad.
    //   ui.theme{ panel_color, panel_alpha, panel_image, panel_slice, border_color, border_alpha, title_color, title_size,
    //             text_color, text_size, muted_color, button_color, accent_color, button_text_size, button_height,
    //             padding, spacing, rounding, slot_color, slot_size, slot_image, slot_slice, tooltip_width }
    //   ui.wants_mouse() -> true when the pointer is over UI (or dragging): don't treat the click as a world click
    //   ui.dragging() -> the dragged value or nil, ui.last_rect() -> x, y, w, h (0..1), ui.text_width(text [, size [, bold]])
    //   ui.show(path [, { vars = {...}, id }]) -> clicked, index    draws a layout made in the UI Editor (Assets/UI/x.json,
    //       Runtime/UIDocument.h) this frame. Its expressions ({=expr} in text, visible, value, disabled, on_click ...)
    //       run in this script's environment, with vars on top and `index` / `item` inside a repeat. clicked: the
    //       name of the button or slot clicked this frame (or the panel whose x was clicked), index its repeat entry.
    //       Edits in the editor show at once while playing. A widget's event = "name" sends events.emit(name,
    //       { ui = path, widget, index }).
    //   ui.simulate_pointer(x, y, down [, wheel [, right]]) / ui.simulate_pointer()   test hook: overrides the pointer
    //   scene.raycast(origin, dir, max_dist [, ignore_entity]) -> nil or { entity, point, normal, distance }
    //   scene.spherecast(origin, dir, max_dist, radius [, ignore_entity [, static_only]]) -> nil or { entity, point, normal,
    //                    distance }   a sphere swept along the ray (distance = how far its centre got; 0 = touching at the start)
    //   scene.screen_ray(x, y) -> origin, dir          through the game camera, x, y 0..1 of the view (ui.mouse())
    //   scene.pick(x, y [, max_dist = 500]) -> nil or a raycast result along screen_ray (what is under the mouse)
    //   scene.world_to_screen(pos) -> x, y, visible, depth     where a world point is on the view (0..1; the inverse
    //       of screen_ray): health bars, names, floating text. Behind the camera x, y are nil and visible false
    //   ---- decal: shapes projected onto the ground (floor, steps, ramps) for this frame only (call every frame)
    //   decal.draw(pos [, { size = 1 or vec3(w, height, d), shape = "circle"|"ring"|"square"|"texture", color, alpha = 0.6,
    //                       rotation (radians), thickness (0 filled, else outline width 0..1), fill (inside alpha of an
    //                       outline), softness = 0.04, emission = 1, height = 1, texture = "Textures/x.png" }])
    //       targets, area templates, movement ranges, path markers. They fade on steep surfaces. decal.count()
    //   ---- foliage: grass and flowers (FoliageComponent). Moving dynamic and kinematic bodies bend it by themselves.
    //   foliage.push(pos [, radius = 0.5, strength = 1])   press the grass around pos this frame (call every frame while
    //       something without a body walks through it); it springs back over the field's Recovery seconds
    //   ---- db: the game database (Assets/Data/database.json, the editor's Window > Database): read-only records
    //   db.item(id), db.npc(id), db.stat(id) -> the record as a table (fields as in the file: name, description, kind,
    //       slot, icon, model, value, stackable, consumable, stats = { name = n }, effects = {...}; NPCs: inventory...)
    //       or nil. db.items(), db.npcs(), db.stats() -> arrays of them. db.death_stat() -> "health"
    //   ---- NPCs (NpcComponent): an entity that is an NPC of the database. It comes to life when the game starts (or
    //   when spawned): stats from the database, its starting inventory rolled. Its state is saved with the level.
    //   e.npc -> nil or Npc: id, name, data (its record), dead, alive
    //     stat(name) -> value with effects and equipment, base(name), set(name, v), add(name, delta)   (set / add return
    //       true if that killed it), damage(n) -> killed, heal(n), kill()
    //     inventory() -> { { item, count, equipped, drop, name }... }, count(item), give(item [, n]) -> added,
    //       take(item [, n]) -> bool, drop(item [, n]) -> the pickup entity next to it (or nil)
    //     equip(item) / unequip(item) -> ok, err (one item per slot: the old one comes off), equipped(slot) -> item or nil
    //     use(item) -> ok, err   its "use" effects; a consumable is used up
    //     apply(item [, trigger = "hit"])   an item's effects of that trigger on this NPC (a weapon hitting it) -> killed
    //     add_effect(stat, amount [, seconds [, over_time]])  +amount for a while (0/nil: for good), or amount per second
    //     effects() -> { { stat, amount, source, remaining (-1 = for good), over_time }... }
    //   Effects of items: add (at once), modifier (+amount while it lasts; equip: while equipped), overTime (per second),
    //   event (events.emit(name, { entity, item, amount })). An NPC dies when the death stat reaches its minimum: what it
    //   carries with "drop" falls to the ground as pickups, every script gets
    //   on_event(self, "npc_died", { entity, npc = id, position, drops = { pickup entities } }) (and the NPC's own death
    //   event if it has one), then the entity is removed (unless the NPC keeps its body: removeOnDeath false).
    //   e:add_npc(id) -> Npc   make this entity that NPC (its looks are not changed)
    //   scene.spawn_npc(id [, pos [, yaw]]) -> entity   a new one with its model, animation graph, script and body
    //   scene.npcs([id [, dead = false]]) -> the NPC entities (of that id; dead ones only if asked)
    //   ---- pickups (PickupComponent): items lying in the world. Picking up is the game's job: give it, destroy it.
    //   e.pickup -> nil or { item, count, name }
    //   scene.pickups([pos [, radius]]) -> pickup entities, nearest first
    //   scene.spawn_pickup(item [, count = 1 [, pos]]) -> entity
    //   ---- terrain: heightmap terrains (TerrainComponent). No physics needed: works in on_create too.
    //   terrain.height(x, z) / terrain.height(pos) -> world y of the ground there, or nil off every terrain
    //   terrain.normal(x, z) / terrain.normal(pos) -> unit vec3 (world), or nil
    //   ---- nav: a layered walkability grid (Navigation/NavGrid.h), baked from the STATIC bodies on first use.
    //   Cells are integer ids. Costs count steps: 1 per orthogonal step, `diagonal` per diagonal one.
    //   nav.bake([{ cell_size=1, agent_radius=0.3, agent_height=1.8, max_step=0.4, max_slope=40, min=vec3, max=vec3 }])
    //                                        -> cell count (bake again after the level changed; bodies added by
    //                                           scripts count from the next frame). min/max: bounds (default: all static bodies)
    //   nav.baked(), nav.cell_size()
    //   nav.find_path(from, to [, { grid=false, diagonal=1.414, closest=true, max_cost }]) -> nil or
    //       { points = { vec3... }, cells = { id... }, cost, complete, costs = { ... } (grid: cost up to each point) }
    //       Free paths go from `from` to exactly `to`, straightened (any angle). grid = true keeps cell centres.
    //       closest: an unreachable (or blocked) goal gives the path to the nearest reachable cell (complete = false).
    //       max_cost cuts the path where it would cost more.
    //   nav.reachable(from (vec3 or cell), budget [, { diagonal }]) -> { { cell, position, cost, previous }... }
    //       every cell within budget, the start first; previous (cell or nil) leads back to the start
    //   nav.cell_at(pos) -> cell or nil       the cell standing under pos (picks the right floor of a column)
    //   nav.nearest(pos [, max_dist=2 [, free=true]]) -> cell or nil   free: skip blocked cells
    //   nav.cell_position(cell) -> vec3, nav.height(pos) -> y or nil, nav.line_clear(a, b) -> bool
    //   nav.set_blocked(cell, bool), nav.is_blocked(cell), nav.clear_blocked()   units, closed doors (paths avoid them)
    //   nav.line_of_sight(from, to [, { eye = 1.5, target = 1.0 }]) -> visible, hit_point   static level only (heights
    //       above the points: the looker's eyes, the target's chest)
    //   nav.cover(at, from) -> "none"|"half"|"full", 0|1|2   an obstacle next to `at` towards `from`: blocks waist height
    //       (half) or head height too (full)
    //   nav.visible_cells(from, range [, { eye, target }]) -> { { cell, position }... }   what `from` can see within range
    //   ---- fog: fog of war on the nav grid's columns (black = never seen, dimmed = explored, clear = visible now)
    //   fog.enable([{ hidden_color, explored_color, explored_alpha = 0.6, fade = 4 }]) -> ok, fog.disable(), fog.enabled()
    //   fog.reveal(pos, radius [, eye = 1.6])   call every frame for each viewer: columns within radius it has line of
    //       sight to (chest height, static level only) are visible this frame and become explored
    //   fog.explore(pos, radius)   explored without sight (a map, a scouting spell); fog.reset() forgets everything
    //   fog.is_visible(pos), fog.is_explored(pos)   (true when fog is off); hide units in the fog with e.visible
    //   fog.save() -> string, fog.load(string) -> ok   the explored map, for on_save / on_load
    //   input.key_down(k), input.key_pressed(k), input.key_released(k)    Key.A..Key.Z, Key.Num0..9, Key.F1..12,
    //   input.mouse_down(b), input.mouse_pressed(b), input.mouse_released(b), input.mouse_position()  Key.Space, Key.Left ... Mouse.Left ...
    //   input.wheel() -> this frame's mouse wheel over the view (notches; positive = scrolled up / away)
    //   ---- input actions: named in Assets/Data/input.json (format in Core/InputMap.h), fed by keys, mouse and gamepad
    //   input.action(name) -> down, input.pressed(name), input.released(name)   (any action type: down = strength >= 0.5)
    //   input.value(name) -> button 0..1 | axis -1..1 | vector length 0..1, input.vector(name) -> x, y (y = up / forward)
    //   input.actions() -> { names }, input.action_type(name), input.bindings(name) -> { "key:Space", "pad:A" } (button)
    //       or { positive, negative } (axis) or { up, down, left, right } (vector)
    //   input.device() -> "keyboard" | "gamepad"   the one used last (for button prompts)
    //   input.label(name) -> "Jump" (input.json "label", else from the name), input.rebindable(name) (input.json "rebind")
    //   input.set_bindings(name, { "key:F", "pad:Y" } | { up = {...}, left = {...} })   the player's bindings: replaces
    //       those slots (only the slots given), on top of input.json; kept for the rest of the run, and the Game
    //       saves them with its settings. input.default_bindings(name) -> as input.json has them (same shape as bindings)
    //   input.reset_bindings([name])   back to input.json (one action, or all)
    //   input.gamepad() -> connected, name; input.pad_down(b), pad_pressed(b), pad_released(b)   "A", "Start", "DPadUp"...
    //   input.pad_axis(a) -> value   "LeftX", "LeftY", "RightX", "RightY" (-1..1, Y up), "LT", "RT" (0..1); dead zones applied
    //   input.simulate_key(k, down), input.simulate_gamepad({ A = true, LeftX = 0.7 } | nil)   tests (from the next frame)
    //   An unknown action, button or axis name is a script error.
    //   shared                                  a table all scripts see (score, game state...)
    //   log(...), print(...)
    class ScriptEngine {
    public:
        ScriptEngine(std::string assetDir, std::string engineDir, PhysicsWorld* physics, AudioEngine* audio);
        ~ScriptEngine();
        ScriptEngine(const ScriptEngine&) = delete;
        ScriptEngine& operator=(const ScriptEngine&) = delete;

        // Name <-> resource mapping used by add_mesh and mesh.geometry
        void SetResources(const ResourceRegistry& resources);
        void SetPrefabs(PrefabLibrary* prefabs);   // for scene.spawn_prefab
        void SetNavigation(NavGrid* nav);          // for nav.*
        void SetFog(FogOfWar* fog);                // for fog.*
        void SetViewAspect(float aspect);          // width / height of the game view, for scene.screen_ray
        void SetHost(ScriptHost* host);            // for saves.* and level.*

        // Script state for a save game: { "game": {...}, "shared": {...}, "scripts": { "<uid>": on_save(self) } }.
        // include: which entities' scripts to ask (by uid; null = all). Errors in on_save are reported and skipped.
        nlohmann::json CaptureState(const std::function<bool(uint64_t)>& include = nullptr);
        // The next Start restores this: `game` and `shared` before any on_create, and each entity's
        // on_load(self, data) right after its on_create. loading: scene.is_loading() is true meanwhile.
        void SetRestore(nlohmann::json state, bool loading);

        void Start(Scene& scene);              // call when Play starts (after PhysicsWorld::Sync)
        void Update(Scene& scene, float dt);   // call once per simulated frame, before the physics step
        void DispatchCollisions(Scene& scene); // call after the physics step: delivers its contact events
        void DispatchAnimationEvents(Scene& scene);  // call after Scene::OnUpdate: on_anim_event and "sound:" events
        void Stop(Scene& scene);               // call before the scene is restored on Stop
        void Reload(Scene& scene);             // Stop + Start, keeping the scene as it is (hot reload)

        bool IsRunning() const;
        uint32_t InstanceCount() const;        // scripts currently running
        const std::vector<std::string>& RecentErrors() const;   // newest last, cleared on Start
        const std::vector<ScriptUIItem>& UIItems() const;        // this frame's ui.* calls, in order
        // Where the pointer is over the game view (0..1, top left origin) and whether the left button
        // is down. Call every frame before Update: ui.button and ui.mouse read it.
        void SetPointer(float x, float y, bool inside, bool down, float wheel = 0.0f, bool rightDown = false);
        // The fonts laid-out UI measures text with (ui.label wraps, buttons fit their label). Null: ImGui's default.
        void SetUIFonts(ImFont* bold, ImFont* regular);
        // Typing reaches script text fields (ui.input) only while allowed: the editor allows it while its Viewport has
        // the keyboard and no ImGui text box does. WantsKeyboard: a text field has it (or had it when this frame
        // began), so the app should leave Escape / Enter / shortcuts alone.
        void SetKeyboard(bool allowed);
        bool WantsKeyboard() const;
        // A closable laid-out panel was up last frame (or a script asked ui.back()): Escape / pad B is the UI's Back,
        // so the apps don't also pause / stop Play on it
        bool WantsBack() const;

        // The player's bindings: overrides on top of Assets/Data/input.json (format in Core/InputMap.h). Kept across
        // Start / Stop (levels, restarts); applied at once while running, else at the next Start. InputOverrides is
        // what applied (scripts change it too, with input.set_bindings), for the app to save. InputActions is the
        // live map, DefaultInputActions the project's (both read at Start).
        void SetInputOverrides(const nlohmann::json& overrides);
        const nlohmann::json& InputOverrides() const;
        const InputMap& InputActions() const;
        const InputMap& DefaultInputActions() const;

        bool ConsumeQuitRequest();                             // true once after scene.quit()
        bool ConsumeRestartRequest();                           // true once after scene.restart()

        struct Impl;                           // keeps sol2 out of everything else (named here so helpers in the .cpp can use it)
    private:
        std::unique_ptr<Impl> m_Impl;
    };

} // namespace Engine
