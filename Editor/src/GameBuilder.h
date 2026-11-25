#pragma once
#include "AssetReferences.h"
#include "Core/Project.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// File > Build Game: makes a folder you can zip and ship, on a background thread.
// Windows:
//   1. compiles Game.exe in Release (cmake --build <build dir> --config Release --target Game), or, in a
//      packaged editor (package_editor), copies the prebuilt Player/Game.exe next to MDBEditor.exe
//   2. <output>/<title>/: <title>.exe, EngineAssets/, the project's used Assets/ (without its "exclude" files)
//      and project.json, so the exe finds everything next to itself
// Web (WebGL2 in a browser, Game/web):
//   1. builds the web player (Tools/build_web.ps1: Emscripten into <source>/build-web), or, packaged, copies
//      Player/Web (Game.html, Game.js, Game.wasm)
//   2. <output>/<title> (Web)/: index.html, Game.js, Game.wasm, game.pak (the same files as the Windows build, in one
//      package: format in Game/web/shell.html) and Play.bat + serve.ps1 (a local web server: browsers don't run
//      the game from file://). Upload the folder to a web host (itch.io: zip it, kind HTML) to share it.
// An existing output folder is replaced only if it looks like an earlier build (project.json / game.pak).
// Only the assets the game uses are shipped (AssetRefs::FindUsedAssets, plus project.json's "include"; "allAssets"
// ships everything), minus "exclude". Check() works that out before building and looks for trouble.
class GameBuilder {
public:
    enum class Target { Windows, Web };
    ~GameBuilder();

    // What a build would ship, and what is wrong first. Errors stop the build; warnings are worth reading.
    struct Check {
        std::vector<std::string> Errors, Warnings, Notes;
        AssetRefs::UsedAssets Used;
        std::vector<std::string> Ship;          // files under Assets to ship (relative, sorted)
        std::vector<std::string> LeftOut;       // files under Assets that won't be (unused or excluded)
        size_t Excluded = 0;                    // of LeftOut: by "exclude"
        uintmax_t ShipBytes = 0, AllBytes = 0;
        size_t AllFiles = 0;
        bool Ok() const { return Errors.empty(); }
    };
    static Check CheckBuild(const Engine::Project& project, const std::string& engineDir, const std::string& outputDir, Target target);

    // False if a build is already running. Ships check.Ship from the project's Assets.
    bool Start(const Engine::Project& project, const std::string& engineDir, const std::string& outputDir, Target target, const Check& check);

    bool Running() const { return m_Running; }
    bool Done() const { return m_Done; }                 // finished (either way) since the last Start
    bool Succeeded() const { return m_Succeeded; }
    Target BuiltTarget() const { return m_Target; }
    std::string GameFolder() const;                      // <output>/<title>/ (valid once started)
    std::string ExePath() const;                         // Windows: the exe; web: Play.bat
    std::vector<std::string> Lines() const;              // the log so far (cmake's output and the copy steps)

    static std::string SafeName(const std::string& title);   // the title without characters Windows refuses
    static std::string FolderName(const std::string& title, Target target);   // "<title>" or "<title> (Web)"

    // game.pak: "MDBPAK01", the uint32 length of a JSON index [[path, offset, size], ...], the index, the bytes.
    // files: (path in the package, file on disk). False with the reason in *error.
    static bool WritePak(const std::string& pakPath, const std::vector<std::pair<std::string, std::string>>& files,
                         uintmax_t* bytes, std::string* error);

private:
    void Run(Engine::Project project, std::string engineDir, std::string outputDir, std::vector<std::string> assets, size_t leftOut);
    void RunWeb(Engine::Project project, std::string engineDir, std::string outputDir, std::vector<std::string> assets, size_t leftOut);
    void Add(const std::string& line);
    void Fail(const std::string& why);

    std::thread m_Thread;
    std::atomic<bool> m_Running{ false }, m_Done{ false }, m_Succeeded{ false };
    Target m_Target = Target::Windows;
    mutable std::mutex m_Mutex;
    std::vector<std::string> m_Lines;
    std::string m_GameFolder, m_Exe;
};
