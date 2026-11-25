#pragma once
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Asset references across a project, so renaming, moving or deleting a file doesn't silently break what uses it.
//
// References stay paths (the files remain readable, and scripts name assets by path too). Every text file of the
// project is scanned for string values that name an asset:
//   - JSON (.json, .mat, .shadergraph): any string value that is a path under Assets, also with a prefix
//     ("model:Models/Fox.glb", "texture:...", "sound:...") or a fragment ("model:Models/x.glb#lod1"). Two short forms
//     are known: project.json's "scene" (relative to Assets/Scenes) and a scene file's environment "map" (relative to
//     Assets/Environments).
//   - glTF (.gltf): "uri" values, relative to the .gltf's own folder (so a moved .gltf keeps finding its .bin and
//     images, and a moved image is found by every .gltf that uses it).
//   - Lua (.lua): string literals, the same way as JSON, plus a bare "x.json" as a scene in Assets/Scenes
//     (level.change). Comments and long strings are not rewritten.
// Text that names a moved file without being such a reference (an entity called "Exit: cellar.json > Yard", a
// comment, a prefab spawned by name "coin", a dialogue found by an entity's name) is listed as a MENTION: shown so it
// can be checked by hand, never changed.
//
// Paths are relative to Assets with forward slashes; project files are named relative to the project folder
// ("Assets/Scenes/demo.json", "project.json").
namespace AssetRefs {

    // A file or a folder going from one path to another. A folder takes everything under it along.
    struct Move { std::string From, To; };

    // Where a path ends up: To for From itself, To + the rest for a path under a moved folder; nullopt if no move applies
    std::optional<std::string> MovedPath(const std::string& path, const std::vector<Move>& moves);

    // A reference found in a file
    struct Use {
        std::string File;          // relative to the project folder
        int Line = 0;              // 1-based
        std::string Text;          // as written ("model:Models/Fox.glb")
        std::string Target;        // the asset it names ("Models/Fox.glb")
        std::string NewText;       // after the moves (Plan only; empty for a mention)
        std::string Note;          // why a mention isn't rewritten
    };

    // What moving would do. Made by PlanMoves, carried out by Apply.
    struct Plan {
        std::vector<Move> Moves;
        bool MoveFiles = true;         // false: only the references change (relinking to files already there)
        std::vector<Use> Changes;      // rewritten
        std::vector<Use> Mentions;     // not rewritten: check by hand
        std::vector<std::string> ChangedFiles;   // project-relative, before the moves
        std::vector<std::string> Problems;       // why it can't be done ("Models/x.glb already exists")

        struct Edit { size_t Begin = 0, End = 0; std::string Old, Text; };   // bytes of the file to replace (Old: what they were)
        std::map<std::string, std::vector<Edit>> Edits;                 // by project-relative file (before the moves)
    };

    // projectDir and assetDir are absolute and end in '/' (assetDir = projectDir + "Assets/")
    Plan PlanMoves(const std::string& projectDir, const std::string& assetDir, std::vector<Move> moves, bool moveFiles = true);

    // Move the files (with Unity-style ".meta" files beside them), then rewrite the references (each file through a
    // temporary file and a rename). Returns false if nothing was done (the reasons are in errors); a reference file
    // that can't be written is reported in errors and the rest still goes ahead.
    bool Apply(const Plan& plan, const std::string& projectDir, const std::string& assetDir, std::vector<std::string>& errors);

    // Every reference to `target` (a file, or anything under it for a folder), and the mentions of it
    struct Uses { std::vector<Use> References, Mentions; };
    Uses FindUses(const std::string& projectDir, const std::string& assetDir, const std::string& target);

    // References to files that don't exist, grouped by the missing path, with the files of the same name that do
    struct Missing {
        std::string Target;
        std::vector<Use> Uses;
        std::vector<std::string> Candidates;   // existing assets with the same file name (relink to one of them)
    };
    std::vector<Missing> FindMissing(const std::string& projectDir, const std::string& assetDir);

    // The assets a game uses (Build Game ships only these). Starting from project.json (start scene, fonts), the files
    // the engine always reads (Data/database.json, Data/input.json, Scripts/Lib/*) and the files matching `include`
    // patterns, every reached file is followed:
    //   - text files (scenes, prefabs, materials, shader graphs, UI layouts, dialogues, Lua, .gltf and a .glb's JSON
    //     chunk, .mtl): their references as above, and every word that names an asset file ("Exit: cellar.json > Yard",
    //     a comment naming "step.wav": shipping too much is safe, too little breaks the game)
    //   - Lua: a string that is a prefab's or dialogue's bare name ("coin", "sage"), and a string naming a folder
    //     ("Dialogues/", from "Dialogues/" .. name .. ".json"): the whole folder is kept
    //   - scenes: an entity whose name is a dialogue's (npc.lua plays Dialogues/<name in lower case>.json)
    //   - models: <stem>.clips.json and kit.json beside them; .obj / .fbx / .dae: the images in their folder
    // Missing lists references from used files to files that don't exist.
    struct UsedAssets {
        std::vector<std::string> Files;               // relative to Assets, as on disk, sorted
        std::map<std::string, std::string> Why;      // file -> what brought it in ("Scenes/a.json:12", "always read")
        std::vector<std::string> Unused;              // every other file under Assets
        std::vector<Missing> Missing;
        std::vector<std::string> Notes;               // folders kept whole, ...
    };
    // engineDir: EngineAssets/ (its Scripts/Lib runs in every game and may name project files)
    UsedAssets FindUsedAssets(const std::string& projectDir, const std::string& assetDir, const std::string& engineDir,
                              const std::vector<std::string>& include);

    // ---- the pieces, exposed for tests
    // A path made relative to a folder ("Models/Village", "Textures/a.png" -> "../../Textures/a.png")
    std::string RelativeTo(const std::string& folder, const std::string& path);
    // "a/./b/../c" -> "a/c"; empty if it leaves the root
    std::string Normalize(const std::string& path);
    // Rewrite one JSON text's string values with the moves (the history's scene keys); unchanged if none apply
    std::string RewriteJsonText(const std::string& text, const std::vector<Move>& moves);
    // A stored reference after the moves ("model:Models/a.glb#lod1" -> "model:Models/b.glb#lod1"); nullopt if unchanged
    std::optional<std::string> MovedReference(const std::string& ref, const std::vector<Move>& moves);

} // namespace AssetRefs
