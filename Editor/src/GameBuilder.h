#pragma once
#include "Core/Project.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// File > Build Game: makes a folder you can zip and ship, on a background thread.
//   1. compiles Game.exe in Release (cmake --build <build dir> --config Release --target Game), or, in a
//      packaged editor (package_editor), copies the prebuilt Player/Game.exe next to MDBEditor.exe
//   2. <output>/<title>/: <title>.exe, EngineAssets/, the project's Assets/ (without its "exclude" files)
//      and project.json, so the exe finds everything next to itself
// An existing <output>/<title> folder is replaced only if it looks like an earlier build (it has project.json).
class GameBuilder {
public:
    ~GameBuilder();

    // False if a build is already running
    bool Start(const Engine::Project& project, const std::string& engineDir, const std::string& outputDir);

    bool Running() const { return m_Running; }
    bool Done() const { return m_Done; }                 // finished (either way) since the last Start
    bool Succeeded() const { return m_Succeeded; }
    std::string GameFolder() const;                      // <output>/<title>/ (valid once started)
    std::string ExePath() const;
    std::vector<std::string> Lines() const;              // the log so far (cmake's output and the copy steps)

    static std::string SafeName(const std::string& title);   // the title without characters Windows refuses

private:
    void Run(Engine::Project project, std::string engineDir, std::string outputDir);
    void Add(const std::string& line);

    std::thread m_Thread;
    std::atomic<bool> m_Running{ false }, m_Done{ false }, m_Succeeded{ false };
    mutable std::mutex m_Mutex;
    std::vector<std::string> m_Lines;
    std::string m_GameFolder, m_Exe;
};
