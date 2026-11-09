#include "Projects.h"
#include "Platform.h"

#include "Core/Application.h"
#include "Core/Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Projects {

    namespace {
        std::string Env(const char* name) {
            char* value = nullptr;
            size_t len = 0;
            std::string out;
            if (_dupenv_s(&value, &len, name) == 0 && value) out = value;
            std::free(value);
            return out;
        }

        fs::path RecentFile() {
            const std::string app = Env("APPDATA");
            return app.empty() ? fs::path("projects.json") : fs::u8path(app) / "MDB Engine" / "projects.json";
        }

        std::vector<std::string> ReadRecent() {
            std::vector<std::string> out;
            std::ifstream in(RecentFile());
            if (!in) return out;
            try {
                nlohmann::json j;
                in >> j;
                for (const auto& p : j.value("recent", nlohmann::json::array()))
                    if (p.is_string()) out.push_back(p.get<std::string>());
            }
            catch (...) {}
            return out;
        }

        void WriteRecent(const std::vector<std::string>& list) {
            std::error_code ec;
            fs::create_directories(RecentFile().parent_path(), ec);
            std::ofstream out(RecentFile());
            if (out) out << nlohmann::json{ { "recent", list } }.dump(2) << "\n";
        }

        // A packaged editor (cmake --target package_editor) has EngineAssets and Examples next to its exe.
        // A build-tree editor has neither and uses the source tree's folders compiled in.
        std::string BaseDir() {
            static const std::string dir = [] {
                const std::string exe = Platform::ExeDir();
                std::error_code ec;
                return fs::is_directory(fs::u8path(exe) / "EngineAssets" / "Shaders", ec)
                    ? Engine::Project::Normalize(exe) : std::string(MDB_SOURCE_DIR);
            }();
            return dir;
        }

        bool Same(const std::string& a, const std::string& b) {
            std::error_code ec;
            return Engine::Project::Normalize(a) == Engine::Project::Normalize(b)
                || fs::equivalent(fs::u8path(a), fs::u8path(b), ec);
        }

        Engine::Project Resolve() {
            const auto& args = Engine::Application::GetCommandLineArgs();
            std::string folder;
            bool remember = true;
            for (size_t i = 1; i + 1 < args.size(); i++) if (args[i] == "--project") folder = args[i + 1];
            if (folder.empty()) {
                folder = Env("MDB_PROJECT");
                if (!folder.empty()) {
                    remember = false;                                   // test runs leave the recent list alone
                    if (!Engine::Project::IsProject(folder) && Engine::Project::IsProject(BaseDir() + folder))
                        folder = BaseDir() + folder;
                }
            }
            if (folder.empty()) {
                for (const auto& r : ReadRecent()) if (Engine::Project::IsProject(r)) { folder = r; break; }
            }
            if (folder.empty()) folder = ExamplesDir();

            Engine::Project p;
            std::string error;
            if (!Engine::Project::Load(folder, p, &error)) {
                LOG_ERROR("Can't open the project: {}. Opening the Examples project instead.", error);
                if (!Engine::Project::Load(ExamplesDir(), p, &error)) {
                    LOG_ERROR("{}", error);
                    p.Root = Engine::Project::Normalize(ExamplesDir());
                }
            }
            if (remember) AddRecent(p.Root);
            LOG_INFO("Project: {} ({})", p.Title, p.Root);
            return p;
        }

        Engine::Project& Storage() {
            static Engine::Project project = Resolve();
            return project;
        }
    }

    const Engine::Project& Current() { return Storage(); }
    Engine::Project& CurrentMutable() { return Storage(); }

    bool Packaged() { return BaseDir() != std::string(MDB_SOURCE_DIR); }

    const std::string& EngineDir() {
        static const std::string dir = Engine::Project::Normalize(Packaged() ? BaseDir() + "EngineAssets" : std::string(ENGINE_ASSET_DIR));
        return dir;
    }

    const std::string& ExamplesDir() {
        static const std::string dir = Engine::Project::Normalize(Packaged() ? BaseDir() + "Examples" : std::string(EXAMPLES_DIR));
        return dir;
    }

    bool IsExamples(const Engine::Project& project) { return Same(project.Root, ExamplesDir()); }

    std::vector<std::string> Recent() {
        std::vector<std::string> out;
        for (const auto& r : ReadRecent()) if (Engine::Project::IsProject(r)) out.push_back(r);
        return out;
    }

    void AddRecent(const std::string& root) {
        auto list = ReadRecent();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& r) { return Same(r, root); }), list.end());
        list.insert(list.begin(), Engine::Project::Normalize(root));
        if (list.size() > 10) list.resize(10);
        WriteRecent(list);
    }

    void RemoveRecent(const std::string& root) {
        auto list = ReadRecent();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& r) { return Same(r, root); }), list.end());
        WriteRecent(list);
    }

} // namespace Projects
