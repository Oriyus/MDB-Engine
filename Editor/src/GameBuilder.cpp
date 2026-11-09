#include "GameBuilder.h"
#include "Platform.h"

#include "Core/Log.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

GameBuilder::~GameBuilder() {
    if (m_Thread.joinable()) m_Thread.join();     // a build that is running finishes first
}

std::string GameBuilder::SafeName(const std::string& title) {
    std::string out;
    for (char c : title) if (std::string("<>:\"/\\|?*").find(c) == std::string::npos && (unsigned char)c >= 32) out += c;
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? std::string("Game") : out;
}

std::string GameBuilder::GameFolder() const { std::lock_guard lock(m_Mutex); return m_GameFolder; }
std::string GameBuilder::ExePath() const { std::lock_guard lock(m_Mutex); return m_Exe; }
std::vector<std::string> GameBuilder::Lines() const { std::lock_guard lock(m_Mutex); return m_Lines; }

void GameBuilder::Add(const std::string& line) {
    std::lock_guard lock(m_Mutex);
    m_Lines.push_back(line);
}

bool GameBuilder::Start(const Engine::Project& project, const std::string& engineDir, const std::string& outputDir) {
    if (m_Running) return false;
    if (m_Thread.joinable()) m_Thread.join();
    {
        std::lock_guard lock(m_Mutex);
        m_Lines.clear();
        const std::string name = SafeName(project.Title);
        m_GameFolder = Engine::Project::Normalize(outputDir) + name + "/";
        m_Exe = m_GameFolder + name + ".exe";
    }
    m_Done = false;
    m_Succeeded = false;
    m_Running = true;
    m_Thread = std::thread(&GameBuilder::Run, this, project, engineDir, outputDir);
    return true;
}

void GameBuilder::Run(Engine::Project project, std::string engineDir, std::string outputDir) {
    auto fail = [this](const std::string& why) {
        Add("FAILED: " + why);
        LOG_ERROR("Build Game failed: {}", why);
        m_Succeeded = false;
        m_Done = true;
        m_Running = false;
    };

    // 1. Game.exe in Release: a packaged editor ships it prebuilt (Player/Game.exe next to MDBEditor.exe);
    //    an editor run from the build tree compiles it
    std::error_code ec;
    fs::path builtExe = fs::u8path(Platform::ExeDir()) / "Player" / "Game.exe";
    if (fs::is_regular_file(builtExe, ec)) {
        Add("Using the prebuilt player " + Engine::Project::PathString(builtExe));
    }
    else {
        Add("Compiling Game.exe (Release). The first time this builds the whole engine and takes a few minutes.");
        const std::string cmd = std::string("\"") + MDB_CMAKE_COMMAND + "\" --build \"" + MDB_BUILD_DIR + "\" --config Release --target Game";
        Add("> " + cmd);
        const int code = Platform::RunProcess(cmd, [this](const std::string& line) {
            // MSBuild prints a lot; keep what tells progress or trouble
            if (line.find("error") != std::string::npos || line.find("warning C") != std::string::npos
                || line.find("->") != std::string::npos || line.find("Building") != std::string::npos
                || line.find("Compiling") != std::string::npos || line.find(".cpp") != std::string::npos)
                Add("  " + line);
        });
        if (code != 0) { fail(code < 0 ? "could not run CMake (" + std::string(MDB_CMAKE_COMMAND) + ")" : "the compile failed (exit code " + std::to_string(code) + ")"); return; }
        builtExe = fs::u8path(std::string(MDB_BUILD_DIR) + "bin/Release/Game.exe");
        if (!fs::exists(builtExe, ec)) { fail("no Game.exe at " + Engine::Project::PathString(builtExe)); return; }
    }

    // 2. The folder
    const std::string name = SafeName(project.Title);
    const fs::path out = fs::u8path(GameFolder());
    if (fs::exists(out, ec)) {
        if (!fs::exists(out / "project.json", ec)) { fail(Engine::Project::PathString(out) + " exists and is not an earlier build; pick another output folder"); return; }
        Add("Replacing the earlier build in " + Engine::Project::PathString(out));
        fs::remove_all(out, ec);
        if (ec) { fail("can't remove the earlier build (is the game still running?): " + ec.message()); return; }
    }
    fs::create_directories(out, ec);
    if (ec) { fail("can't create " + Engine::Project::PathString(out) + ": " + ec.message()); return; }

    fs::copy_file(builtExe, out / fs::u8path(name + ".exe"), ec);
    if (ec) { fail("can't copy Game.exe: " + ec.message()); return; }

    size_t files = 0, skipped = 0;
    uintmax_t bytes = 0;
    auto copyTree = [&](const fs::path& from, const fs::path& to, bool useExclude) -> bool {
        for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string rel = Engine::Project::PathString(fs::relative(it->path(), from, ec));
            if (useExclude && project.IsExcluded(rel)) { skipped++; continue; }
            const fs::path dest = to / fs::u8path(rel);
            fs::create_directories(dest.parent_path(), ec);
            fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
            if (ec) { fail("can't copy " + rel + ": " + ec.message()); return false; }
            files++;
            bytes += fs::file_size(it->path(), ec);
        }
        if (ec) { fail("can't read " + Engine::Project::PathString(from) + ": " + ec.message()); return false; }
        return true;
    };
    Add("Copying the engine files (shaders, Lua libraries)");
    if (!copyTree(fs::u8path(engineDir), out / "EngineAssets", false)) return;
    Add("Copying the project's Assets");
    if (!copyTree(fs::u8path(project.AssetDir()), out / "Assets", true)) return;
    fs::copy_file(fs::u8path(project.File()), out / "project.json", ec);
    if (ec) { fail("can't copy project.json: " + ec.message()); return; }

    char size[64];
    std::snprintf(size, sizeof(size), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    Add("Done: " + std::to_string(files) + " files, " + size + (skipped ? ", " + std::to_string(skipped) + " left out by \"exclude\"" : std::string()));
    Add("Ship the folder " + Engine::Project::PathString(out) + " (zip it). Players need the Microsoft Visual C++ Redistributable (x64), which most PCs have.");
    LOG_INFO("Game built in {}", Engine::Project::PathString(out));
    m_Succeeded = true;
    m_Done = true;
    m_Running = false;
}
