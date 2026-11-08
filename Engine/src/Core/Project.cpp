#include "Core/Project.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Engine {

    std::string Project::Normalize(const std::string& folder) {
        std::error_code ec;
        fs::path p = fs::absolute(fs::u8path(folder), ec).lexically_normal();
        std::string s = PathString(p);
        while (s.size() > 3 && s.back() == '/') s.pop_back();     // "D:/x/" -> "D:/x" (keep "D:/")
        if (s.empty() || s.back() != '/') s += '/';
        return s;
    }

    bool Project::IsProject(const std::string& folder) {
        std::error_code ec;
        return !folder.empty() && fs::is_regular_file(fs::u8path(Normalize(folder) + "project.json"), ec);
    }

    bool Project::Load(const std::string& folder, Project& out, std::string* error) {
        Project p;
        p.Root = Normalize(folder);
        std::ifstream in(fs::u8path(p.File()));
        if (!in) { if (error) *error = "no project.json in " + p.Root; return false; }
        try {
            nlohmann::json j;
            in >> j;
            p.Title = j.value("title", p.Title);
            p.Scene = j.value("scene", p.Scene);
            p.Width = j.value("width", p.Width);
            p.Height = j.value("height", p.Height);
            p.Fullscreen = j.value("fullscreen", p.Fullscreen);
            p.VSync = j.value("vsync", p.VSync);
            if (j.contains("exclude") && j["exclude"].is_array())
                for (const auto& e : j["exclude"]) if (e.is_string()) p.Exclude.push_back(e.get<std::string>());
        }
        catch (const std::exception& ex) {
            if (error) *error = p.File() + " is malformed: " + ex.what();
            return false;
        }
        out = std::move(p);
        return true;
    }

    bool Project::Save(std::string* error) const {
        nlohmann::ordered_json j;
        j["title"] = Title;
        j["scene"] = Scene;
        j["width"] = Width;
        j["height"] = Height;
        j["fullscreen"] = Fullscreen;
        j["vsync"] = VSync;
        j["exclude"] = Exclude;
        std::ofstream out(fs::u8path(File()));
        if (!out) { if (error) *error = "can't write " + File(); return false; }
        out << j.dump(2) << "\n";
        return true;
    }

    bool Project::Create(const std::string& folder, const std::string& title, Project& out, std::string* error) {
        if (IsProject(folder)) { if (error) *error = Normalize(folder) + " already holds a project"; return false; }
        Project p;
        p.Root = Normalize(folder);
        p.Title = title.empty() ? PathString(fs::u8path(p.Root.substr(0, p.Root.size() - 1)).filename()) : title;
        std::error_code ec;
        for (const char* sub : { "Scenes", "Scripts", "Models", "Textures", "Sounds", "Prefabs", "Dialogues", "Environments" }) {
            fs::create_directories(fs::u8path(p.AssetDir() + sub), ec);
            if (ec) { if (error) *error = "can't create " + p.AssetDir() + sub + ": " + ec.message(); return false; }
        }
        if (!p.Save(error)) return false;
        if (!fs::exists(fs::u8path(p.Root + ".gitignore"), ec))
            std::ofstream(fs::u8path(p.Root + ".gitignore")) << "# Build Game output\nBuild/\n";
        out = std::move(p);
        return true;
    }

    // '*' matches any run of characters (slashes too); everything else must match exactly (case-insensitive)
    bool Project::Matches(const std::string& path, const std::string& pattern) {
        auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
        size_t p = 0, s = 0, star = std::string::npos, mark = 0;
        while (s < path.size()) {
            if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = s; }
            else if (p < pattern.size() && lower(pattern[p]) == lower(path[s])) { p++; s++; }
            else if (star != std::string::npos) { p = star + 1; s = ++mark; }
            else return false;
        }
        while (p < pattern.size() && pattern[p] == '*') p++;
        return p == pattern.size();
    }

    bool Project::IsExcluded(const std::string& path) const {
        for (const auto& pattern : Exclude) if (Matches(path, pattern)) return true;
        return false;
    }

} // namespace Engine
