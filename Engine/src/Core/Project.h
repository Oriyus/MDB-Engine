#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Engine {

    // A game project: a folder holding project.json and Assets/ (Scenes, Scripts, Models...). The editor opens
    // one project at a time; Game.exe plays one (a packaged game has project.json next to the exe).
    // The engine's own files (shaders, Scripts/Lib) live apart, in the engine asset folder (EngineAssets/).
    //
    // project.json (every field optional):
    //   { "title": "My Game", "scene": "main.json", "width": 1280, "height": 720, "fullscreen": false,
    //     "vsync": true, "exclude": ["Scripts/*_test.lua"] }
    // "scene" is the start scene (in Assets/Scenes). "exclude" lists files under Assets that Build Game
    // leaves out ('*' matches anything, including '/').
    struct Project {
        std::string Root;                      // absolute, '/' separators, ending in '/'
        std::string Title = "MDB Game";
        std::string Scene = "main.json";
        uint32_t Width = 1280, Height = 720;
        bool Fullscreen = false, VSync = true;
        std::vector<std::string> Exclude;

        std::string AssetDir() const { return Root + "Assets/"; }
        std::string File() const { return Root + "project.json"; }

        static bool IsProject(const std::string& folder);    // has a project.json
        // Reads <folder>/project.json (a missing file is an error; a malformed one too)
        static bool Load(const std::string& folder, Project& out, std::string* error = nullptr);
        bool Save(std::string* error = nullptr) const;
        // A new project: the folder (made if needed, must not hold a project already), project.json,
        // Assets with the usual subfolders, and a .gitignore for Build/
        static bool Create(const std::string& folder, const std::string& title, Project& out, std::string* error = nullptr);

        // True if `path` (relative to Assets, '/' separators) matches any Exclude pattern
        bool IsExcluded(const std::string& path) const;
        static bool Matches(const std::string& path, const std::string& pattern);
        // The folder as Root expects it: absolute, '/' separators, a trailing '/'
        static std::string Normalize(const std::string& folder);
        // A path as UTF-8 with '/' separators (C++20's u8string is a different type from std::string)
        static std::string PathString(const std::filesystem::path& p) { auto s = p.generic_u8string(); return std::string(s.begin(), s.end()); }
    };

} // namespace Engine
