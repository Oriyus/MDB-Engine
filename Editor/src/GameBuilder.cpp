#include "GameBuilder.h"
#include "Platform.h"

#include "Core/Log.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include <nlohmann/json.hpp>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

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

std::string GameBuilder::FolderName(const std::string& title, Target target) {
    return SafeName(title) + (target == Target::Web ? " (Web)" : "");
}

void GameBuilder::Fail(const std::string& why) {
    Add("FAILED: " + why);
    LOG_ERROR("Build Game failed: {}", why);
    m_Succeeded = false;
    m_Done = true;
    m_Running = false;
}

GameBuilder::Check GameBuilder::CheckBuild(const Engine::Project& project, const std::string& engineDir, const std::string& outputDir, Target target) {
    Check c;
    std::error_code ec;
    const std::string assetDir = project.AssetDir();

    // The start scene
    if (project.Scene.empty()) c.Errors.push_back("No start scene is set.");
    else if (!fs::is_regular_file(fs::u8path(assetDir + "Scenes/" + project.Scene), ec))
        c.Errors.push_back("The start scene Scenes/" + project.Scene + " doesn't exist.");
    else if (project.IsExcluded("Scenes/" + project.Scene))
        c.Errors.push_back("The start scene Scenes/" + project.Scene + " is left out by \"Leave out\".");

    // The output folder: replaced only if it is an earlier build
    const fs::path out = fs::u8path(Engine::Project::Normalize(outputDir) + FolderName(project.Title, target) + "/");
    if (fs::exists(out, ec) && !fs::exists(out / (target == Target::Web ? "game.pak" : "project.json"), ec))
        c.Errors.push_back(Engine::Project::PathString(out) + " exists and is not an earlier build: pick another output folder.");
    if (Engine::Project::Normalize(outputDir).rfind(project.AssetDir(), 0) == 0)
        c.Errors.push_back("The output folder is inside Assets: the build would ship itself next time.");

    // What to ship
    c.Used = AssetRefs::FindUsedAssets(project.Root, assetDir, engineDir, project.Include);
    std::vector<std::string> all = c.Used.Files;
    all.insert(all.end(), c.Used.Unused.begin(), c.Used.Unused.end());
    std::sort(all.begin(), all.end());
    c.AllFiles = all.size();
    std::set<std::string> used(c.Used.Files.begin(), c.Used.Files.end());
    std::vector<std::string> usedExcluded;
    for (const std::string& rel : all) {
        const uintmax_t size = fs::file_size(fs::u8path(assetDir + rel), ec);
        const uintmax_t bytes = ec ? 0 : size;
        c.AllBytes += bytes;
        const bool wanted = project.AllAssets || used.count(rel);
        if (wanted && project.IsExcluded(rel)) {
            c.Excluded++;
            c.LeftOut.push_back(rel);
            if (used.count(rel)) usedExcluded.push_back(rel);
            continue;
        }
        if (!wanted) { c.LeftOut.push_back(rel); continue; }
        c.Ship.push_back(rel);
        c.ShipBytes += bytes;
    }
    for (const std::string& rel : usedExcluded)
        c.Warnings.push_back(rel + " is used (" + c.Used.Why[rel] + ") but left out by \"Leave out\".");

    // The engine's files: what the player reads (shaders, fonts, script libraries, anything new) always; the sample
    // model (Models/) only if the game uses the built-in "model" geometry; the shader graph presets (ShaderGraphs/)
    // never: the editor copies a preset into the project's Assets when one is used. "Ship every asset" ships all of
    // them. Editor window layouts (editor.ini, imgui.ini) are never shipped.
    {
        const fs::path engine = fs::u8path(engineDir);
        for (auto it = fs::recursive_directory_iterator(engine, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string rel = Engine::Project::PathString(fs::relative(it->path(), engine, ec));
            const uintmax_t size = fs::file_size(it->path(), ec);
            const uintmax_t bytes = ec ? 0 : size;
            std::string why;
            const std::string name = Engine::Project::PathString(it->path().filename());
            if (name == "editor.ini" || name == "imgui.ini") why = "an editor window layout";
            else if (!project.AllAssets) {
                if (rel.rfind("ShaderGraphs/", 0) == 0) why = "the editor's shader graph presets";
                else if (rel.rfind("Models/", 0) == 0 && !c.Used.BuiltinModel) why = "the built-in sample model (geometry \"model\"), not used";
            }
            if (why.empty()) { c.EngineShip.push_back(rel); c.EngineBytes += bytes; }
            else { c.EngineLeftOut.push_back(rel); c.EngineWhy[rel] = why; c.EngineLeftOutBytes += bytes; }
        }
        if (ec) c.Errors.push_back("Can't read the engine's files in " + engineDir + ": " + ec.message());
        ec.clear();
        std::sort(c.EngineShip.begin(), c.EngineShip.end());
        std::sort(c.EngineLeftOut.begin(), c.EngineLeftOut.end());
        if (c.Used.BuiltinModel) c.Notes.push_back("Shipping the built-in sample model EngineAssets/Models/BoomBox.glb: " + c.Used.BuiltinModelWhy + " uses geometry \"model\".");
    }

    // References to files that aren't there
    for (const auto& m : c.Used.Missing) {
        const auto& u = m.Uses.front();
        std::string line = u.File + ":" + std::to_string(u.Line) + " names " + m.Target + ", which doesn't exist";
        if (m.Uses.size() > 1) line += " (and " + std::to_string(m.Uses.size() - 1) + " more uses)";
        if (!m.Candidates.empty()) line += "; a file of that name: " + m.Candidates.front() + " (Assets > Check References... relinks)";
        c.Warnings.push_back(line + ".");
    }

    // Files the game reads must parse
    lua_State* L = luaL_newstate();
    for (const std::string& rel : c.Ship) {
        std::string ext = fs::u8path(rel).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
        const bool json = ext == ".json" || ext == ".mat" || ext == ".shadergraph" || ext == ".gltf";
        if (!json && ext != ".lua") continue;
        std::ifstream in(fs::u8path(assetDir + rel), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (json) {
            if (!nlohmann::json::accept(text)) c.Errors.push_back(rel + " is not valid JSON: the game can't read it.");
        }
        else if (L) {
            if (luaL_loadbufferx(L, text.data(), text.size(), ("@" + rel).c_str(), "t") != LUA_OK) {
                const char* msg = lua_tostring(L, -1);
                c.Warnings.push_back(std::string("Script syntax error: ") + (msg ? msg : rel) + " (the script will be disabled in the game).");
            }
            lua_settop(L, 0);
        }
    }
    if (L) lua_close(L);

    if (project.AllAssets) c.Notes.push_back("\"Ship every asset\" is on: unused files are shipped too.");
    c.Notes.insert(c.Notes.end(), c.Used.Notes.begin(), c.Used.Notes.end());
    return c;
}

bool GameBuilder::Start(const Engine::Project& project, const std::string& engineDir, const std::string& outputDir, Target target, const Check& check) {
    if (m_Running) return false;
    if (m_Thread.joinable()) m_Thread.join();
    {
        std::lock_guard lock(m_Mutex);
        m_Lines.clear();
        m_GameFolder = Engine::Project::Normalize(outputDir) + FolderName(project.Title, target) + "/";
        m_Exe = m_GameFolder + (target == Target::Web ? std::string("Play.bat") : SafeName(project.Title) + ".exe");
    }
    m_Target = target;
    m_Done = false;
    m_Succeeded = false;
    m_Running = true;
    const size_t leftOut = check.LeftOut.size();
    m_Thread = target == Target::Web ? std::thread(&GameBuilder::RunWeb, this, project, engineDir, outputDir, check.Ship, check.EngineShip, leftOut)
                                     : std::thread(&GameBuilder::Run, this, project, engineDir, outputDir, check.Ship, check.EngineShip, leftOut);
    return true;
}

void GameBuilder::Run(Engine::Project project, std::string engineDir, std::string outputDir, std::vector<std::string> assets,
                      std::vector<std::string> engineFiles, size_t leftOut) {
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

    size_t files = 0;
    uintmax_t bytes = 0;
    auto copyOne = [&](const fs::path& from, const fs::path& dest, const std::string& rel) -> bool {
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(from, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) { fail("can't copy " + rel + ": " + ec.message()); return false; }
        files++;
        bytes += fs::file_size(from, ec);
        return true;
    };
    Add("Copying the " + std::to_string(engineFiles.size()) + " engine files the game needs (shaders, fonts, Lua libraries)");
    const fs::path engine = fs::u8path(engineDir);
    for (const std::string& rel : engineFiles)
        if (!copyOne(engine / fs::u8path(rel), out / "EngineAssets" / fs::u8path(rel), "EngineAssets/" + rel)) return;
    Add("Copying the " + std::to_string(assets.size()) + " assets the game uses");
    for (const std::string& rel : assets)
        if (!copyOne(fs::u8path(project.AssetDir() + rel), out / "Assets" / fs::u8path(rel), rel)) return;
    fs::copy_file(fs::u8path(project.File()), out / "project.json", ec);
    if (ec) { fail("can't copy project.json: " + ec.message()); return; }

    char size[64];
    std::snprintf(size, sizeof(size), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    Add("Done: " + std::to_string(files) + " files, " + size + (leftOut ? ", " + std::to_string(leftOut) + " unused or excluded assets left out" : std::string()));
    Add("Ship the folder " + Engine::Project::PathString(out) + " (zip it). Players need the Microsoft Visual C++ Redistributable (x64), which most PCs have.");
    LOG_INFO("Game built in {}", Engine::Project::PathString(out));
    m_Succeeded = true;
    m_Done = true;
    m_Running = false;
}

bool GameBuilder::WritePak(const std::string& pakPath, const std::vector<std::pair<std::string, std::string>>& files,
                           uintmax_t* bytes, std::string* error) {
    std::error_code ec;
    // The index first: JSON [[path, offset, size], ...] with offsets from the end of the index
    std::string index = "[";
    uintmax_t offset = 0;
    for (size_t i = 0; i < files.size(); i++) {
        const uintmax_t size = fs::file_size(fs::u8path(files[i].second), ec);
        if (ec) { if (error) *error = "can't read " + files[i].second + ": " + ec.message(); return false; }
        std::string path;
        for (char c : files[i].first) { if (c == '"' || c == '\\') path += '\\'; path += c; }
        index += (i ? ",[\"" : "[\"") + path + "\"," + std::to_string(offset) + "," + std::to_string(size) + "]";
        offset += size;
    }
    index += "]";
    std::ofstream out(fs::u8path(pakPath), std::ios::binary | std::ios::trunc);
    if (!out) { if (error) *error = "can't write " + pakPath; return false; }
    const uint32_t length = (uint32_t)index.size();
    const unsigned char le[4] = { (unsigned char)(length & 0xFF), (unsigned char)((length >> 8) & 0xFF),
                                  (unsigned char)((length >> 16) & 0xFF), (unsigned char)((length >> 24) & 0xFF) };
    out.write("MDBPAK01", 8);
    out.write(reinterpret_cast<const char*>(le), 4);
    out.write(index.data(), (std::streamsize)index.size());
    std::vector<char> buffer(1 << 20);
    for (const auto& file : files) {
        std::ifstream in(fs::u8path(file.second), std::ios::binary);
        if (!in) { if (error) *error = "can't read " + file.second; return false; }
        while (in) {
            in.read(buffer.data(), (std::streamsize)buffer.size());
            out.write(buffer.data(), in.gcount());
        }
    }
    if (!out) { if (error) *error = "writing " + pakPath + " failed (disk full?)"; return false; }
    if (bytes) *bytes = offset;
    return true;
}

void GameBuilder::RunWeb(Engine::Project project, std::string engineDir, std::string outputDir, std::vector<std::string> assets,
                         std::vector<std::string> engineFiles, size_t leftOut) {
    (void)outputDir;
    std::error_code ec;
    // 1. The web player: a packaged editor ships it (Player/Web, with Play.bat and serve.ps1); an editor run from the
    //    source tree builds it with Emscripten (Tools/build_web.ps1 into <source>/build-web)
    fs::path player = fs::u8path(Platform::ExeDir()) / "Player" / "Web";
    fs::path launcher = player;
    if (fs::is_regular_file(player / "Game.wasm", ec)) {
        Add("Using the prebuilt web player " + Engine::Project::PathString(player));
    }
    else {
        const std::string source = MDB_SOURCE_DIR;
        Add("Building the web player with Emscripten (Tools/build_web.ps1). The first time compiles the engine for the web: several minutes.");
        const std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + source + "Tools/build_web.ps1\"";
        Add("> " + cmd);
        const int code = Platform::RunProcess(cmd, [this](const std::string& line) {
            if (line.find("rror") != std::string::npos || line.find("Emscripten") != std::string::npos
                || line.find("Linking") != std::string::npos || line.find("Configuring") != std::string::npos
                || line.find("not found") != std::string::npos || line.find(".cpp") != std::string::npos)
                Add("  " + line);
        });
        if (code != 0) {
            Fail(code < 0 ? std::string("could not run PowerShell")
                          : "the web build failed (exit code " + std::to_string(code) + "). Is Emscripten installed? See Tools/build_web.ps1");
            return;
        }
        player = fs::u8path(source + "build-web/bin");
        launcher = fs::u8path(source + "Game/web");
        if (!fs::is_regular_file(player / "Game.wasm", ec)) { Fail("no web player at " + Engine::Project::PathString(player)); return; }
    }

    // 2. The folder
    const fs::path out = fs::u8path(GameFolder());
    if (fs::exists(out, ec)) {
        if (!fs::exists(out / "game.pak", ec)) { Fail(Engine::Project::PathString(out) + " exists and is not an earlier web build; pick another output folder"); return; }
        Add("Replacing the earlier build in " + Engine::Project::PathString(out));
        fs::remove_all(out, ec);
        if (ec) { Fail("can't remove the earlier build (is its server still running?): " + ec.message()); return; }
    }
    fs::create_directories(out, ec);
    if (ec) { Fail("can't create " + Engine::Project::PathString(out) + ": " + ec.message()); return; }

    // The files, as the Windows build lays them out next to its exe
    std::vector<std::pair<std::string, std::string>> files;
    for (const std::string& rel : engineFiles)
        files.emplace_back("EngineAssets/" + rel, Engine::Project::Normalize(engineDir) + rel);
    Add("Packing the engine files and the " + std::to_string(assets.size()) + " assets the game uses into game.pak");
    for (const std::string& rel : assets) files.emplace_back("Assets/" + rel, project.AssetDir() + rel);
    files.emplace_back("project.json", project.File());
    uintmax_t bytes = 0;
    std::string error;
    if (!WritePak(Engine::Project::PathString(out / "game.pak"), files, &bytes, &error)) { Fail(error); return; }

    // The page (titled after the game), the player and the launcher
    {
        std::ifstream in(player / "Game.html", std::ios::binary);
        std::string html((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (html.empty()) { Fail("can't read Game.html"); return; }
        std::string title;
        for (char c : project.Title) {
            if (c == '<') title += "&lt;"; else if (c == '>') title += "&gt;"; else if (c == '&') title += "&amp;"; else title += c;
        }
        const std::string from = "<title>MDB Game</title>";
        if (const size_t at = html.find(from); at != std::string::npos) html.replace(at, from.size(), "<title>" + title + "</title>");
        std::ofstream page(out / "index.html", std::ios::binary);
        page << html;
        if (!page) { Fail("can't write index.html"); return; }
    }
    for (const char* f : { "Game.js", "Game.wasm" }) {
        fs::copy_file(player / f, out / f, ec);
        if (ec) { Fail(std::string("can't copy ") + f + ": " + ec.message()); return; }
    }
    for (const char* f : { "Play.bat", "serve.ps1" }) {
        fs::copy_file(launcher / f, out / f, ec);
        if (ec) { Fail(std::string("can't copy ") + f + " (from " + Engine::Project::PathString(launcher) + "): " + ec.message()); return; }
    }

    char size[64];
    std::snprintf(size, sizeof(size), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    Add("Done: " + std::to_string(files.size()) + " files in game.pak, " + size + (leftOut ? ", " + std::to_string(leftOut) + " unused or excluded assets left out" : std::string()));
    Add("Play it with Play.bat in " + Engine::Project::PathString(out) + " (a small local web server: browsers don't run it from a file).");
    Add("To share it, upload the folder to a web host. itch.io: zip the folder, project kind HTML.");
    LOG_INFO("Web game built in {}", Engine::Project::PathString(out));
    m_Succeeded = true;
    m_Done = true;
    m_Running = false;
}
