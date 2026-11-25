#include "AssetReferences.h"
#include "Core/Project.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace AssetRefs {

namespace {

    std::string U8(const std::u8string& s) { return std::string(s.begin(), s.end()); }

    std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    std::string FolderOf(const std::string& rel) {
        const size_t slash = rel.rfind('/');
        return slash == std::string::npos ? std::string() : rel.substr(0, slash);
    }

    std::string NameOf(const std::string& rel) {
        const size_t slash = rel.rfind('/');
        return slash == std::string::npos ? rel : rel.substr(slash + 1);
    }

    std::string ExtOf(const std::string& rel) {
        const std::string name = NameOf(rel);
        const size_t dot = name.rfind('.');
        return dot == std::string::npos || dot == 0 ? std::string() : Lower(name.substr(dot));
    }

    std::string StemOf(const std::string& rel) {
        const std::string name = NameOf(rel);
        const size_t dot = name.rfind('.');
        return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    }

    bool StartsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

    // Files that can be asset references (for the missing-file check)
    bool IsAssetExtension(const std::string& ext) {
        static const std::set<std::string> exts = {
            ".glb", ".gltf", ".fbx", ".obj", ".bin", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".lua", ".wav", ".mp3",
            ".flac", ".ogg", ".hdr", ".mat", ".shadergraph", ".json", ".terrain", ".ies", ".vert", ".frag", ".glsl", ".comp" };
        return exts.count(ext) > 0;
    }

    // ---------------------------------------------------------------- the project's text files
    enum class FileKind { Json, Gltf, Lua, Plain };

    std::optional<FileKind> TextKind(const std::string& ext) {
        if (ext == ".json" || ext == ".mat" || ext == ".shadergraph") return FileKind::Json;
        if (ext == ".gltf") return FileKind::Gltf;
        if (ext == ".lua") return FileKind::Lua;
        if (ext == ".mtl" || ext == ".txt" || ext == ".ini" || ext == ".csv") return FileKind::Plain;   // mentions only
        return std::nullopt;
    }

    struct TextFile {
        std::string Rel;              // relative to the project folder
        std::string AssetRel;         // relative to Assets ("" for project.json)
        FileKind Kind = FileKind::Plain;
        std::string Text;
        std::vector<size_t> LineStarts;

        int LineOf(size_t pos) const {
            return (int)(std::upper_bound(LineStarts.begin(), LineStarts.end(), pos) - LineStarts.begin());
        }
        std::string LineText(size_t pos) const {
            const int line = LineOf(pos);
            const size_t b = LineStarts[(size_t)line - 1];
            size_t e = Text.find('\n', b);
            if (e == std::string::npos) e = Text.size();
            std::string s = Text.substr(b, e - b);
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            size_t lead = 0;
            while (lead < s.size() && (s[lead] == ' ' || s[lead] == '\t')) lead++;
            s = s.substr(lead);
            if (s.size() > 140) {                         // a long line (a minified file): the part round the hit
                const size_t at = pos - b > lead ? pos - b - lead : 0;
                const size_t from = at > 60 ? at - 60 : 0;
                s = (from ? "..." : "") + s.substr(from, 140) + "...";
            }
            return s;
        }
    };

    bool ReadFile(const fs::path& p, std::string& out) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    // Every text file of the project: project.json, then everything under Assets that can hold references
    void ForEachTextFile(const std::string& projectDir, const std::string& assetDir, const std::function<void(const TextFile&)>& f) {
        std::error_code ec;
        auto load = [&](const fs::path& p, std::string rel, std::string assetRel, FileKind kind) {
            if (fs::file_size(p, ec) > 64ull * 1024 * 1024 || ec) return;   // not a file to scan
            TextFile t;
            t.Rel = std::move(rel);
            t.AssetRel = std::move(assetRel);
            t.Kind = kind;
            if (!ReadFile(p, t.Text)) return;
            t.LineStarts.push_back(0);
            for (size_t i = 0; i < t.Text.size(); i++) if (t.Text[i] == '\n') t.LineStarts.push_back(i + 1);
            f(t);
        };
        const fs::path project = fs::u8path(projectDir + "project.json");
        if (fs::is_regular_file(project, ec)) load(project, "project.json", "", FileKind::Json);
        const fs::path root = fs::u8path(assetDir);
        std::vector<std::pair<fs::path, std::string>> files;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            files.push_back({ it->path(), U8(fs::relative(it->path(), root, ec).generic_u8string()) });
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        for (const auto& [p, rel] : files)
            if (auto kind = TextKind(ExtOf(rel))) load(p, "Assets/" + rel, rel, *kind);
    }

    // ---------------------------------------------------------------- string tokens
    struct Token {
        size_t Begin = 0, End = 0;    // the contents, without the quotes
        std::string Key;              // JSON: the key this value belongs to (the last one seen)
        bool IsKey = false;           // JSON: an object key (never rewritten)
        bool Escaped = false;         // holds a backslash: not read as a path
    };

    std::vector<Token> JsonStrings(const std::string& t) {
        std::vector<Token> out;
        std::string lastKey;
        size_t i = 0;
        while (i < t.size()) {
            if (t[i] != '"') { i++; continue; }
            Token k;
            k.Begin = i + 1;
            size_t j = k.Begin;
            while (j < t.size() && t[j] != '"') {
                if (t[j] == '\\') { k.Escaped = true; j++; }
                j++;
            }
            if (j >= t.size()) break;
            k.End = j;
            size_t n = j + 1;
            while (n < t.size() && std::isspace((unsigned char)t[n])) n++;
            if (n < t.size() && t[n] == ':') { k.IsKey = true; lastKey = t.substr(k.Begin, k.End - k.Begin); }
            else k.Key = lastKey;
            out.push_back(std::move(k));
            i = j + 1;
        }
        return out;
    }

    // Lua: [[ ]] / [==[ ]==] at pos -> the position after its end (npos if it isn't one)
    size_t LongBracketEnd(const std::string& t, size_t pos) {
        if (pos >= t.size() || t[pos] != '[') return std::string::npos;
        size_t p = pos + 1, level = 0;
        while (p < t.size() && t[p] == '=') { level++; p++; }
        if (p >= t.size() || t[p] != '[') return std::string::npos;
        const std::string close = "]" + std::string(level, '=') + "]";
        const size_t e = t.find(close, p + 1);
        return e == std::string::npos ? t.size() : e + close.size();
    }

    std::vector<Token> LuaStrings(const std::string& t) {
        std::vector<Token> out;
        size_t i = 0;
        while (i < t.size()) {
            const char c = t[i];
            if (c == '-' && i + 1 < t.size() && t[i + 1] == '-') {          // a comment
                const size_t e = LongBracketEnd(t, i + 2);
                if (e != std::string::npos) { i = e; continue; }
                const size_t nl = t.find('\n', i);
                i = nl == std::string::npos ? t.size() : nl + 1;
                continue;
            }
            if (c == '[') {                                                  // a long string: not read
                const size_t e = LongBracketEnd(t, i);
                if (e != std::string::npos) { i = e; continue; }
                i++;
                continue;
            }
            if (c == '"' || c == '\'') {
                Token k;
                k.Begin = i + 1;
                size_t j = k.Begin;
                while (j < t.size() && t[j] != c && t[j] != '\n') {
                    if (t[j] == '\\') { k.Escaped = true; j++; }
                    j++;
                }
                if (j >= t.size() || t[j] != c) { i = j; continue; }         // unfinished
                k.End = j;
                out.push_back(std::move(k));
                i = j + 1;
                continue;
            }
            i++;
        }
        return out;
    }

    std::vector<Token> TokensOf(const TextFile& f) {
        switch (f.Kind) {
        case FileKind::Json: case FileKind::Gltf: return JsonStrings(f.Text);
        case FileKind::Lua: return LuaStrings(f.Text);
        default: return {};
        }
    }

    // ---------------------------------------------------------------- what a string names
    enum class Base { Assets, Scenes, Environments, Relative };

    struct Ref {
        std::string Target;           // relative to Assets
        std::string Prefix, Suffix;   // "model:", "#lod1"
        Base From = Base::Assets;
        bool Percent = false;         // a glTF uri written with %20 escapes
        bool Strong = false;          // surely a reference (prefix, folder or known key), for the missing-file check
    };

    std::string PercentDecode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
                out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
                i += 2;
            }
            else out += s[i];
        }
        return out;
    }

    std::string PercentEncode(const std::string& s) {
        std::string out;
        static const char* hex = "0123456789ABCDEF";
        for (unsigned char c : s) {
            if (c == ' ' || c == '%' || c == '#' || c == '?' || c >= 0x80) { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += (char)c;
        }
        return out;
    }

    // The generic form: [prefix:]path[#fragment], where path has an extension
    std::optional<Ref> GenericRef(const std::string& value) {
        Ref r;
        std::string rest = value;
        const size_t colon = rest.find(':');
        if (colon != std::string::npos && colon >= 2 &&
            std::all_of(rest.begin(), rest.begin() + (std::ptrdiff_t)colon, [](unsigned char c) { return std::islower(c) || c == '_'; })) {
            r.Prefix = rest.substr(0, colon + 1);
            rest = rest.substr(colon + 1);
        }
        if (const size_t hash = rest.find('#'); hash != std::string::npos) { r.Suffix = rest.substr(hash); rest = rest.substr(0, hash); }
        if (rest.empty() || rest.front() == '/' || rest.find("://") != std::string::npos || rest.find('\\') != std::string::npos) return std::nullopt;
        if (rest.find_first_of(":\n\t*?") != std::string::npos) return std::nullopt;     // (globs: project.json's exclude)
        if (ExtOf(rest).empty() || Normalize(rest) != rest) return std::nullopt;
        r.Target = rest;
        r.Strong = !r.Prefix.empty() || rest.find('/') != std::string::npos;
        return r;
    }

    std::optional<Ref> Interpret(const TextFile& f, const Token& tok, const std::string& value) {
        if (tok.IsKey || tok.Escaped || value.empty() || value.size() > 1024) return std::nullopt;
        if (f.Kind == FileKind::Gltf) {
            if (tok.Key != "uri" || StartsWith(value, "data:") || value.find("://") != std::string::npos) return std::nullopt;
            const std::string folder = FolderOf(f.AssetRel);
            const std::string target = Normalize((folder.empty() ? "" : folder + "/") + PercentDecode(value));
            if (target.empty()) return std::nullopt;
            Ref r;
            r.Target = target;
            r.From = Base::Relative;
            r.Percent = value.find('%') != std::string::npos;
            r.Strong = true;
            return r;
        }
        if (f.Kind == FileKind::Json && f.Rel == "project.json" && tok.Key == "scene") {
            Ref r; r.Target = "Scenes/" + value; r.From = Base::Scenes; r.Strong = true;
            return r;
        }
        if (f.Kind == FileKind::Json && StartsWith(f.AssetRel, "Scenes/") && tok.Key == "map") {
            Ref r; r.Target = "Environments/" + value; r.From = Base::Environments; r.Strong = true;
            return r;
        }
        auto r = GenericRef(value);
        if (!r) return std::nullopt;
        // Lua: a bare "x.json" is a scene (level.change, scene.load)
        if (f.Kind == FileKind::Lua && r->Prefix.empty() && r->Suffix.empty() && r->Target.find_first_of("/ ") == std::string::npos &&
            ExtOf(r->Target) == ".json") {
            r->Target = "Scenes/" + r->Target;
            r->From = Base::Scenes;
            r->Strong = true;
        }
        return r;
    }

    // How a reference is written for its new target (newFolder: a glTF's folder after the moves)
    std::optional<std::string> Format(const Ref& r, const std::string& target, const std::string& newFolder, std::string& note) {
        switch (r.From) {
        case Base::Assets: return r.Prefix + target + r.Suffix;
        case Base::Scenes:
            if (StartsWith(target, "Scenes/")) return target.substr(7);
            note = "named relative to Assets/Scenes, and the file leaves that folder";
            return std::nullopt;
        case Base::Environments:
            if (StartsWith(target, "Environments/")) return target.substr(13);
            note = "sky maps load from Assets/Environments, and the file leaves that folder";
            return std::nullopt;
        case Base::Relative: {
            const std::string rel = RelativeTo(newFolder, target);
            return r.Percent ? PercentEncode(rel) : rel;
        }
        }
        return std::nullopt;
    }

    // Text that names one of the moved things without being a reference: a file's name (or a folder's path) anywhere,
    // a prefab's or dialogue's name as a Lua string. Positions inside references are skipped (they were handled).
    void FindMentions(const TextFile& f, const std::vector<std::pair<size_t, size_t>>& refRanges,
        const std::vector<std::string>& targets, std::vector<Use>& out) {
        std::set<int> lines;
        for (const Use& u : out) if (u.File == f.Rel) lines.insert(u.Line);
        auto inRef = [&](size_t pos) {
            for (const auto& [b, e] : refRanges) if (pos >= b && pos < e) return true;
            return false;
        };
        auto add = [&](size_t pos, const std::string& target, const std::string& note) {
            const int line = f.LineOf(pos);
            if (!lines.insert(line).second) return;
            Use u;
            u.File = f.Rel;
            u.Line = line;
            u.Text = f.LineText(pos);
            u.Target = target;
            u.Note = note;
            out.push_back(std::move(u));
        };
        for (const std::string& target : targets) {
            const bool folder = ExtOf(target).empty();
            std::vector<std::pair<std::string, std::string>> needles;          // text, note
            if (folder) needles.push_back({ target + "/", "names the folder" });
            else needles.push_back({ NameOf(target), "names the file" });
            if (f.Kind == FileKind::Lua && StartsWith(target, "Prefabs/") && ExtOf(target) == ".json") {
                needles.push_back({ "\"" + StemOf(target) + "\"", "may spawn the prefab by name" });
                needles.push_back({ "'" + StemOf(target) + "'", "may spawn the prefab by name" });
            }
            if (f.Kind == FileKind::Lua && StartsWith(target, "Dialogues/") && ExtOf(target) == ".json") {
                needles.push_back({ "\"" + StemOf(target) + "\"", "may open the dialogue by name" });
                needles.push_back({ "'" + StemOf(target) + "'", "may open the dialogue by name" });
            }
            for (const auto& [needle, note] : needles) {
                if (needle.size() < 3) continue;
                for (size_t p = f.Text.find(needle); p != std::string::npos; p = f.Text.find(needle, p + needle.size()))
                    if (!inRef(p)) add(p, target, note);
            }
            // A scene entity named like a dialogue: npc.lua plays Dialogues/<name in lower case>.json
            if (!folder && StartsWith(target, "Dialogues/") && f.Kind == FileKind::Json && StartsWith(f.AssetRel, "Scenes/")) {
                const std::string stem = Lower(StemOf(target));
                for (const Token& t : JsonStrings(f.Text))
                    if (!t.IsKey && t.Key == "name" && Lower(f.Text.substr(t.Begin, t.End - t.Begin)) == stem)
                        add(t.Begin, target, "an entity of this name may talk with this dialogue (npc.lua)");
            }
        }
    }

    bool ValidName(const std::string& rel) {
        if (rel.empty() || rel.front() == '/' || rel.back() == '/') return false;
        if (rel.find_first_of("\\:*?\"<>|") != std::string::npos) return false;
        return Normalize(rel) == rel;
    }

} // namespace

// ---------------------------------------------------------------- paths
std::string Normalize(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        const std::string part = path.substr(start, slash - start);
        if (part == "..") { if (parts.empty()) return {}; parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        start = slash + 1;
    }
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

std::string RelativeTo(const std::string& folder, const std::string& path) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        size_t start = 0;
        while (start < s.size()) {
            size_t slash = s.find('/', start);
            if (slash == std::string::npos) slash = s.size();
            if (slash > start) v.push_back(s.substr(start, slash - start));
            start = slash + 1;
        }
        return v;
    };
    const auto a = split(folder), b = split(path);
    size_t i = 0;
    while (i < a.size() && i + 1 < b.size() && a[i] == b[i]) i++;
    std::string out;
    for (size_t k = i; k < a.size(); k++) out += "../";
    for (size_t k = i; k < b.size(); k++) out += b[k] + (k + 1 < b.size() ? "/" : "");
    return out;
}

std::optional<std::string> MovedPath(const std::string& path, const std::vector<Move>& moves) {
    for (const Move& m : moves) {
        if (path == m.From) return m.To;
        if (path.size() > m.From.size() && path[m.From.size()] == '/' && StartsWith(path, m.From))
            return m.To + path.substr(m.From.size());
    }
    return std::nullopt;
}

std::optional<std::string> MovedReference(const std::string& ref, const std::vector<Move>& moves) {
    auto r = GenericRef(ref);
    if (!r) return std::nullopt;
    auto to = MovedPath(r->Target, moves);
    if (!to) return std::nullopt;
    const std::string out = r->Prefix + *to + r->Suffix;
    return out == ref ? std::nullopt : std::optional<std::string>(out);
}

std::string RewriteJsonText(const std::string& text, const std::vector<Move>& moves) {
    std::string out;
    size_t at = 0;
    for (const Token& t : JsonStrings(text)) {
        if (t.IsKey || t.Escaped) continue;
        auto moved = MovedReference(text.substr(t.Begin, t.End - t.Begin), moves);
        if (!moved) continue;
        out.append(text, at, t.Begin - at);
        out += *moved;
        at = t.End;
    }
    if (at == 0) return text;
    out.append(text, at, std::string::npos);
    return out;
}

// ---------------------------------------------------------------- planning
Plan PlanMoves(const std::string& projectDir, const std::string& assetDir, std::vector<Move> moves, bool moveFiles) {
    Plan plan;
    std::error_code ec;
    moves.erase(std::remove_if(moves.begin(), moves.end(), [](const Move& m) { return m.From == m.To; }), moves.end());
    plan.Moves = moves;
    plan.MoveFiles = moveFiles;
    for (const Move& m : moves) {
        if (!ValidName(m.From) || !ValidName(m.To)) { plan.Problems.push_back("\"" + m.To + "\" is not a valid name"); continue; }
        if (StartsWith(m.To, m.From + "/")) { plan.Problems.push_back("Can't move " + m.From + " into itself"); continue; }
        if (!moveFiles) continue;
        if (!fs::exists(fs::u8path(assetDir + m.From), ec)) plan.Problems.push_back(m.From + " doesn't exist");
        // (a change of case only is the same file on Windows: allowed)
        else if (Lower(m.From) != Lower(m.To) && fs::exists(fs::u8path(assetDir + m.To), ec)) plan.Problems.push_back(m.To + " already exists");
    }
    if (!plan.Problems.empty()) return plan;

    std::vector<std::string> targets;
    for (const Move& m : moves) targets.push_back(m.From);

    ForEachTextFile(projectDir, assetDir, [&](const TextFile& f) {
        std::vector<std::pair<size_t, size_t>> refRanges;
        std::vector<Plan::Edit> edits;
        // A moved .gltf: its relative uris are written from its new folder
        const auto fileMoved = moveFiles && !f.AssetRel.empty() ? MovedPath(f.AssetRel, moves) : std::nullopt;
        const std::string newFolder = FolderOf(fileMoved ? *fileMoved : f.AssetRel);
        for (const Token& tok : TokensOf(f)) {
            const std::string value = f.Text.substr(tok.Begin, tok.End - tok.Begin);
            auto ref = Interpret(f, tok, value);
            if (!ref) continue;
            refRanges.push_back({ tok.Begin, tok.End });
            const auto moved = MovedPath(ref->Target, moves);
            if (!moved && !(ref->From == Base::Relative && fileMoved)) continue;
            std::string note;
            const auto text = Format(*ref, moved ? *moved : ref->Target, newFolder, note);
            Use u;
            u.File = f.Rel;
            u.Line = f.LineOf(tok.Begin);
            u.Text = value;
            u.Target = ref->Target;
            if (!text) { u.Note = note; plan.Mentions.push_back(std::move(u)); continue; }
            if (*text == value) continue;
            u.NewText = *text;
            plan.Changes.push_back(std::move(u));
            edits.push_back({ tok.Begin, tok.End, value, *text });
        }
        if (!edits.empty()) { plan.Edits[f.Rel] = std::move(edits); plan.ChangedFiles.push_back(f.Rel); }
        if (!fileMoved) FindMentions(f, refRanges, targets, plan.Mentions);   // (a moved file naming itself is fine)
    });
    return plan;
}

bool Apply(const Plan& plan, const std::string& projectDir, const std::string& assetDir, std::vector<std::string>& errors) {
    if (!plan.Problems.empty()) { errors.insert(errors.end(), plan.Problems.begin(), plan.Problems.end()); return false; }
    std::error_code ec;
    // 1. Move the files; if one fails, put the ones moved so far back
    if (plan.MoveFiles) {
        std::vector<const Move*> done;
        for (const Move& m : plan.Moves) {
            const fs::path from = fs::u8path(assetDir + m.From), to = fs::u8path(assetDir + m.To);
            fs::create_directories(to.parent_path(), ec);
            fs::rename(from, to, ec);
            if (ec) {
                errors.push_back("Can't move " + m.From + " to " + m.To + ": " + ec.message());
                for (auto it = done.rbegin(); it != done.rend(); ++it)
                    fs::rename(fs::u8path(assetDir + (*it)->To), fs::u8path(assetDir + (*it)->From), ec);
                return false;
            }
            done.push_back(&m);
            const fs::path meta = fs::u8path(assetDir + m.From + ".meta");      // Unity's, beside imported art
            if (fs::is_regular_file(meta, ec)) fs::rename(meta, fs::u8path(assetDir + m.To + ".meta"), ec);
        }
    }
    // 2. Rewrite the references, each file where it is now
    for (const auto& [rel, edits] : plan.Edits) {
        std::string now = rel;
        if (plan.MoveFiles && StartsWith(rel, "Assets/"))
            if (auto moved = MovedPath(rel.substr(7), plan.Moves)) now = "Assets/" + *moved;
        const fs::path file = fs::u8path(projectDir + now);
        std::string text;
        if (!ReadFile(file, text)) { errors.push_back("Can't read " + now); continue; }
        std::vector<Plan::Edit> sorted = edits;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.Begin > b.Begin; });
        bool same = true;
        for (const auto& e : sorted)
            if (e.End > text.size() || text.compare(e.Begin, e.End - e.Begin, e.Old) != 0) { same = false; break; }
        if (!same) { errors.push_back(now + " changed meanwhile: its references were not updated"); continue; }
        for (const auto& e : sorted) text.replace(e.Begin, e.End - e.Begin, e.Text);
        const fs::path temp = fs::u8path(projectDir + now + ".tmp");
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << text;
            if (!out) { errors.push_back("Can't write " + now + ".tmp"); continue; }
        }
        fs::rename(temp, file, ec);
        if (ec) { fs::remove(temp, ec); errors.push_back("Can't replace " + now + ": " + ec.message()); }
    }
    return true;
}

// ---------------------------------------------------------------- uses
Uses FindUses(const std::string& projectDir, const std::string& assetDir, const std::string& target) {
    Uses out;
    ForEachTextFile(projectDir, assetDir, [&](const TextFile& f) {
        std::vector<std::pair<size_t, size_t>> refRanges;
        for (const Token& tok : TokensOf(f)) {
            const std::string value = f.Text.substr(tok.Begin, tok.End - tok.Begin);
            auto ref = Interpret(f, tok, value);
            if (!ref) continue;
            refRanges.push_back({ tok.Begin, tok.End });
            if (ref->Target != target && !StartsWith(ref->Target, target + "/")) continue;
            Use u;
            u.File = f.Rel;
            u.Line = f.LineOf(tok.Begin);
            u.Text = value;
            u.Target = ref->Target;
            out.References.push_back(std::move(u));
        }
        if (f.AssetRel != target && !StartsWith(f.AssetRel, target + "/")) FindMentions(f, refRanges, { target }, out.Mentions);
    });
    return out;
}

std::vector<Missing> FindMissing(const std::string& projectDir, const std::string& assetDir) {
    std::error_code ec;
    // Every asset by file name, for candidates
    std::unordered_map<std::string, std::vector<std::string>> byName;
    const fs::path root = fs::u8path(assetDir);
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) {
            const std::string rel = U8(fs::relative(it->path(), root, ec).generic_u8string());
            byName[Lower(NameOf(rel))].push_back(rel);
        }
    std::map<std::string, Missing> found;
    std::unordered_map<std::string, bool> exists;
    ForEachTextFile(projectDir, assetDir, [&](const TextFile& f) {
        for (const Token& tok : TokensOf(f)) {
            const std::string value = f.Text.substr(tok.Begin, tok.End - tok.Begin);
            auto ref = Interpret(f, tok, value);
            if (!ref || !ref->Strong || !IsAssetExtension(ExtOf(ref->Target))) continue;
            auto [it, fresh] = exists.try_emplace(ref->Target, false);
            if (fresh) it->second = fs::exists(fs::u8path(assetDir + ref->Target), ec);
            if (it->second) continue;
            Missing& m = found[ref->Target];
            m.Target = ref->Target;
            Use u;
            u.File = f.Rel;
            u.Line = f.LineOf(tok.Begin);
            u.Text = value;
            u.Target = ref->Target;
            m.Uses.push_back(std::move(u));
        }
    });
    std::vector<Missing> out;
    for (auto& [target, m] : found) {
        if (auto it = byName.find(Lower(NameOf(target))); it != byName.end()) m.Candidates = it->second;
        std::sort(m.Candidates.begin(), m.Candidates.end());
        out.push_back(std::move(m));
    }
    return out;
}

// ---------------------------------------------------------------- what a game uses
namespace {
    bool LoadText(const fs::path& p, std::string rel, std::string assetRel, FileKind kind, TextFile& t) {
        std::error_code ec;
        if (fs::file_size(p, ec) > 64ull * 1024 * 1024 || ec) return false;
        t.Rel = std::move(rel);
        t.AssetRel = std::move(assetRel);
        t.Kind = kind;
        if (!ReadFile(p, t.Text)) return false;
        if (kind == FileKind::Gltf && ExtOf(t.AssetRel) == ".glb") {         // a .glb: its JSON chunk
            const std::string& s = t.Text;
            auto u32 = [&](size_t at) { return (uint32_t)(unsigned char)s[at] | (uint32_t)(unsigned char)s[at + 1] << 8 |
                                               (uint32_t)(unsigned char)s[at + 2] << 16 | (uint32_t)(unsigned char)s[at + 3] << 24; };
            if (s.size() < 20 || s.compare(0, 4, "glTF") != 0 || u32(16) != 0x4E4F534Au) return false;
            const size_t len = u32(12);
            if (20 + len > s.size()) return false;
            t.Text = s.substr(20, len);
        }
        t.LineStarts.assign(1, 0);
        for (size_t i = 0; i < t.Text.size(); i++) if (t.Text[i] == '\n') t.LineStarts.push_back(i + 1);
        return true;
    }

    bool IsModelExtension(const std::string& ext) {
        return ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj" || ext == ".dae" || ext == ".3ds" || ext == ".blend";
    }
    bool IsImageExtension(const std::string& ext) {
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".dds" || ext == ".hdr" || ext == ".tif" || ext == ".tiff";
    }
}

UsedAssets FindUsedAssets(const std::string& projectDir, const std::string& assetDir, const std::string& engineDir,
                          const std::vector<std::string>& include) {
    UsedAssets out;
    std::error_code ec;

    // Every file under Assets: by path and by name (both in lower case: Windows paths ignore case)
    std::vector<std::string> all;
    std::unordered_map<std::string, std::string> byPath;                  // lower -> as on disk
    std::unordered_map<std::string, std::vector<std::string>> byName;     // lower file name -> paths
    std::set<std::string> folders;                                        // lower, no trailing '/'
    const fs::path root = fs::u8path(assetDir);
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const std::string rel = U8(fs::relative(it->path(), root, ec).generic_u8string());
        if (it->is_directory(ec)) { folders.insert(Lower(rel)); continue; }
        if (!it->is_regular_file(ec)) continue;
        all.push_back(rel);
        byPath[Lower(rel)] = rel;
        byName[Lower(NameOf(rel))].push_back(rel);
    }
    std::sort(all.begin(), all.end());

    std::set<std::string> used;
    std::vector<std::string> queue;
    auto add = [&](const std::string& path, const std::string& why) {
        auto it = byPath.find(Lower(path));
        if (it == byPath.end() || !used.insert(it->second).second) return false;
        out.Why[it->second] = why;
        queue.push_back(it->second);
        return true;
    };
    auto addFolder = [&](const std::string& folder, const std::string& why) {
        const std::string prefix = Lower(folder) + "/";
        size_t n = 0;
        for (const std::string& rel : all)
            if (StartsWith(Lower(rel), prefix) && ExtOf(rel) != ".meta") n += add(rel, why) ? 1 : 0;
        return n;
    };

    std::map<std::string, Missing> missing;
    std::set<std::string> keptFolders;
    auto scan = [&](const TextFile& f) {
        auto where = [&](size_t pos) { return f.Rel + ":" + std::to_string(f.LineOf(pos)); };
        // 1. References, as renames see them
        for (const Token& tok : TokensOf(f)) {
            if (tok.IsKey || tok.Escaped) continue;
            if (f.Rel == "project.json" && (tok.Key == "exclude" || tok.Key == "include")) continue;   // patterns, not uses
            const std::string value = f.Text.substr(tok.Begin, tok.End - tok.Begin);
            if (auto ref = Interpret(f, tok, value)) {
                if (byPath.count(Lower(ref->Target))) add(ref->Target, where(tok.Begin));
                else if (ref->Strong && IsAssetExtension(ExtOf(ref->Target)) && !folders.count(Lower(ref->Target))) {
                    Missing& m = missing[ref->Target];
                    m.Target = ref->Target;
                    Use u;
                    u.File = f.Rel; u.Line = f.LineOf(tok.Begin); u.Text = value; u.Target = ref->Target;
                    m.Uses.push_back(std::move(u));
                }
                continue;
            }
            if (f.Kind == FileKind::Lua && !value.empty() && value.size() < 256) {
                // A folder a script builds names in ("Dialogues/" .. name .. ".json"): keep all of it
                std::string folder = value;
                while (!folder.empty() && folder.back() == '/') folder.pop_back();
                if (value.back() == '/' && !folder.empty() && folders.count(Lower(folder)) && keptFolders.insert(Lower(folder)).second) {
                    const size_t n = addFolder(folder, where(tok.Begin) + " (names built from the folder)");
                    out.Notes.push_back("Kept all of " + folder + "/ (" + std::to_string(n) + (n == 1 ? " file" : " files") + " not found otherwise): " + where(tok.Begin) +
                                        " builds names from it. List what is really needed under \"Always include\" if that is too much.");
                }
                // A bare name: a prefab (scene.spawn_prefab) or a dialogue (dialogue.open)
                else if (value.find_first_of("./ ") == std::string::npos) {
                    add("Prefabs/" + value + ".json", where(tok.Begin));
                    add("Dialogues/" + value + ".json", where(tok.Begin));
                }
            }
            // A scene entity named like a dialogue: npc.lua plays Dialogues/<name in lower case>.json
            if (f.Kind == FileKind::Json && StartsWith(f.AssetRel, "Scenes/") && tok.Key == "name" && value.size() < 128)
                add("Dialogues/" + Lower(value) + ".json", where(tok.Begin) + " (an entity of that name)");
        }
        // 2. Any word inside a string value that names an asset file (a path, or a bare file name: every file of that
        //    name), like an entity called "Exit: cellar.json > Yard". Comments don't count (a Lua comment naming
        //    another script pulled in a whole other game), nor project.json, whose other strings are patterns.
        //    Files without strings (.mtl, .txt): every word.
        if (f.Rel == "project.json") return;
        const std::string& t = f.Text;
        auto wordChar = [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '/' || c >= 0x80; };
        auto words = [&](size_t from, size_t to) {
            for (size_t i = from; i < to;) {
                if (!wordChar((unsigned char)t[i])) { i++; continue; }
                size_t j = i;
                while (j < to && wordChar((unsigned char)t[j])) j++;
                std::string word = t.substr(i, j - i);
                while (!word.empty() && (word.back() == '.' || word.back() == '/')) word.pop_back();
                if (word.size() >= 3 && word.size() < 256 && word.find('.') != std::string::npos && !ExtOf(word).empty() &&
                    !std::isdigit((unsigned char)ExtOf(word)[1])) {
                    if (word.find('/') != std::string::npos) {
                        const std::string n = Normalize(word);
                        if (!n.empty()) add(n, where(i));
                    }
                    else if (auto it = byName.find(Lower(word)); it != byName.end())
                        for (const std::string& rel : it->second) add(rel, where(i) + " (names " + NameOf(rel) + ")");
                }
                i = j;
            }
        };
        if (f.Kind == FileKind::Plain) words(0, t.size());
        else for (const Token& tok : TokensOf(f)) if (!tok.IsKey) words(tok.Begin, tok.End);
    };

    // The roots
    {
        TextFile project;
        if (LoadText(fs::u8path(projectDir + "project.json"), "project.json", "", FileKind::Json, project)) scan(project);
    }
    {   // The engine's script libraries run in every game: what they name is used (the dialogue box's frame, dialogues)
        std::vector<fs::path> libs;
        for (auto it = fs::directory_iterator(fs::u8path(engineDir + "Scripts/Lib"), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && ExtOf(U8(it->path().filename().generic_u8string())) == ".lua") libs.push_back(it->path());
        ec.clear();
        std::sort(libs.begin(), libs.end());
        for (const fs::path& p : libs) {
            TextFile lib;
            if (LoadText(p, "EngineAssets/Scripts/Lib/" + U8(p.filename().generic_u8string()), "", FileKind::Lua, lib)) scan(lib);
        }
    }
    add("Data/database.json", "always read (the game database)");
    add("Data/input.json", "always read (input actions)");
    addFolder("Scripts/Lib", "always read (script libraries)");
    for (const std::string& rel : all)
        for (const std::string& pattern : include)
            if (Engine::Project::Matches(rel, pattern)) { add(rel, "\"Always include\": " + pattern); break; }

    // Follow every file reached
    while (!queue.empty()) {
        const std::string rel = queue.back();
        queue.pop_back();
        const std::string ext = ExtOf(rel), folder = FolderOf(rel), stem = StemOf(rel);
        const std::string inFolder = folder.empty() ? std::string() : folder + "/";
        if (IsModelExtension(ext)) {
            add(inFolder + stem + ".clips.json", rel + " (its clip library)");
            add(inFolder + "kit.json", rel + " (its kit)");
            if (ext == ".obj") add(inFolder + stem + ".mtl", rel + " (its materials)");
            if (ext == ".obj" || ext == ".fbx" || ext == ".dae" || ext == ".3ds" || ext == ".blend") {
                // Formats whose texture names aren't read here: keep the images beside it
                for (const std::string& other : all)
                    if (FolderOf(other) == folder && IsImageExtension(ExtOf(other))) add(other, rel + " (an image beside it)");
            }
        }
        auto kind = TextKind(ext);
        if (ext == ".glb") kind = FileKind::Gltf;
        if (!kind) continue;
        TextFile f;
        if (LoadText(fs::u8path(assetDir + rel), "Assets/" + rel, rel, *kind, f)) scan(f);
    }

    for (const std::string& rel : all) {
        if (used.count(rel)) out.Files.push_back(rel);
        else out.Unused.push_back(rel);
    }
    for (auto& [target, m] : missing) {
        if (auto it = byName.find(Lower(NameOf(target))); it != byName.end()) m.Candidates = it->second;
        std::sort(m.Candidates.begin(), m.Candidates.end());
        out.Missing.push_back(std::move(m));
    }
    return out;
}

} // namespace AssetRefs
