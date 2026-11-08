#include "AssetBrowser.h"
#include "Platform.h"
#include "Core/Log.h"

#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
    std::string Lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    // UTF-8 as a plain std::string (C++20 u8string() returns std::u8string)
    std::string U8(const std::u8string& s) { return std::string(s.begin(), s.end()); }

    std::string Rel(const fs::path& p, const fs::path& root) {
        return U8(fs::relative(p, root).generic_u8string());
    }

    std::string FolderOf(const std::string& rel) {
        const size_t slash = rel.rfind('/');
        return slash == std::string::npos ? std::string() : rel.substr(0, slash);
    }

    std::string NameOf(const std::string& rel) {
        const size_t slash = rel.rfind('/');
        return slash == std::string::npos ? rel : rel.substr(slash + 1);
    }

    // What a new script starts as
    const char* kScriptTemplate =
        "-- %s\n"
        "-- Runs during Play on the entity it is attached to (Inspector > Script, or drag it onto the entity).\n"
        "-- The full API is listed in Engine/src/Scripting/ScriptEngine.h.\n"
        "\n"
        "function on_create(self)\n"
        "end\n"
        "\n"
        "function on_update(self, dt)\n"
        "end\n";

    struct KindStyle { const char* Label; ImU32 Color; };
    KindStyle StyleOf(AssetBrowser::Kind k, const std::string& name) {
        using K = AssetBrowser::Kind;
        std::string ext = Lower(fs::path(name).extension().string());
        if (!ext.empty()) ext = ext.substr(1);
        static thread_local std::string upper;
        upper = ext;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return (char)std::toupper(c); });
        switch (k) {
        case K::Model:       return { upper.c_str(), IM_COL32(214, 140, 64, 255) };
        case K::Prefab:      return { "PREFAB", IM_COL32(74, 150, 240, 255) };
        case K::Script:      return { "LUA", IM_COL32(88, 176, 102, 255) };
        case K::Sound:       return { upper.c_str(), IM_COL32(150, 112, 214, 255) };
        case K::Scene:       return { "SCENE", IM_COL32(60, 176, 190, 255) };
        case K::Environment: return { "HDR", IM_COL32(90, 140, 210, 255) };
        case K::Shader:      return { upper.c_str(), IM_COL32(110, 120, 150, 255) };
        case K::Dialogue:    return { "TALK", IM_COL32(220, 170, 70, 255) };
        case K::Texture:     return { upper.c_str(), IM_COL32(200, 90, 130, 255) };
        default:             return { upper.empty() ? "FILE" : upper.c_str(), IM_COL32(120, 124, 132, 255) };
        }
    }
}

AssetBrowser::AssetBrowser(std::string assetDir) : m_AssetDir(std::move(assetDir)) {
    if (!m_AssetDir.empty() && m_AssetDir.back() != '/' && m_AssetDir.back() != '\\') m_AssetDir += '/';
}

AssetBrowser::Kind AssetBrowser::KindOf(const std::string& path) {
    const std::string ext = Lower(fs::path(path).extension().string());
    if (ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".obj") return Kind::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") return Kind::Texture;
    if (ext == ".lua") return Kind::Script;
    if (ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".ogg") return Kind::Sound;
    if (ext == ".hdr") return Kind::Environment;
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl" || ext == ".comp") return Kind::Shader;
    if (ext == ".json") {
        const std::string folder = Lower(FolderOf(path));
        if (folder.rfind("prefabs", 0) == 0) return Kind::Prefab;
        if (folder.rfind("scenes", 0) == 0) return Kind::Scene;
        if (folder.rfind("dialogues", 0) == 0) return Kind::Dialogue;
    }
    return Kind::Other;
}

const char* AssetBrowser::KindName(Kind k) {
    switch (k) {
    case Kind::Folder: return "Folder";        case Kind::Model: return "Model";
    case Kind::Texture: return "Texture";      case Kind::Prefab: return "Prefab";
    case Kind::Script: return "Script";        case Kind::Sound: return "Sound";
    case Kind::Scene: return "Scene";          case Kind::Environment: return "Sky (HDR)";
    case Kind::Shader: return "Shader";        case Kind::Dialogue: return "Dialogue";
    default: return "File";
    }
}

std::string AssetBrowser::PayloadPath(const ImGuiPayload* payload) {
    if (!payload || !payload->IsDataType(Payload) || payload->DataSize <= 0) return {};
    return std::string((const char*)payload->Data);
}

std::string AssetBrowser::AcceptDrop(std::initializer_list<Kind> kinds) {
    std::string result;
    if (!ImGui::BeginDragDropTarget()) return result;
    // Only light up (and accept) when the dragged asset is a kind this target takes
    const ImGuiPayload* peek = ImGui::GetDragDropPayload();
    const std::string path = PayloadPath(peek);
    if (!path.empty() && std::find(kinds.begin(), kinds.end(), KindOf(path)) != kinds.end())
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(Payload)) result = PayloadPath(p);
    ImGui::EndDragDropTarget();
    return result;
}

// ------------------------------------------------------------------ scanning
void AssetBrowser::Rescan() {
    m_LastScan = ImGui::GetTime();
    m_ForceScan = false;
    const fs::path root = fs::u8path(m_AssetDir);
    std::error_code ec;
    if (!m_Folder.empty() && !fs::is_directory(root / fs::u8path(m_Folder), ec)) m_Folder.clear();

    m_Folders.clear();
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) m_Folders.push_back(Rel(it->path(), root));
    std::sort(m_Folders.begin(), m_Folders.end());

    m_Entries.clear();
    const bool searching = m_Search[0] != '\0';
    const std::string needle = Lower(m_Search);
    auto add = [&](const fs::directory_entry& d) {
        Entry e;
        e.Name = U8(d.path().filename().u8string());
        e.Path = Rel(d.path(), root);
        e.Type = d.is_directory(ec) ? Kind::Folder : KindOf(e.Path);
        e.Size = e.Type == Kind::Folder ? 0 : d.file_size(ec);
        m_Entries.push_back(std::move(e));
    };
    if (searching) {                                   // the whole tree, by name
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (Lower(U8(it->path().filename().u8string())).find(needle) != std::string::npos) add(*it);
    }
    else {
        for (const auto& d : fs::directory_iterator(root / fs::u8path(m_Folder), ec)) add(d);
    }
    std::sort(m_Entries.begin(), m_Entries.end(), [](const Entry& a, const Entry& b) {
        if ((a.Type == Kind::Folder) != (b.Type == Kind::Folder)) return a.Type == Kind::Folder;
        return Lower(a.Name) < Lower(b.Name);
    });
}

std::string AssetBrowser::UniqueName(const std::string& folder, const std::string& base, const std::string& ext) const {
    const fs::path dir = fs::u8path(m_AssetDir) / fs::u8path(folder);
    for (int i = 0; i < 1000; i++) {
        const std::string name = base + (i ? " " + std::to_string(i) : std::string()) + ext;
        if (!fs::exists(dir / fs::u8path(name))) return name;
    }
    return base + ext;
}

// ------------------------------------------------------------------ drawing
std::vector<AssetBrowser::Action> AssetBrowser::Draw(bool* open, const std::function<uint32_t(const std::string&)>& thumbnail) {
    m_Actions.clear();
    if (!ImGui::Begin("Assets", open)) { ImGui::End(); return {}; }
    if (m_ForceScan || ImGui::GetTime() - m_LastScan > 1.0) Rescan();    // picks up files changed outside

    // ---- top row: breadcrumb (left), search, view toggle and tile size (right)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button("Assets")) { m_Folder.clear(); m_Search[0] = '\0'; m_ForceScan = true; }
    std::string walked;
    size_t start = 0;
    while (!m_Folder.empty() && start <= m_Folder.size()) {
        const size_t slash = m_Folder.find('/', start);
        const std::string part = m_Folder.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        walked += (walked.empty() ? "" : "/") + part;
        ImGui::SameLine(0, 2);
        ImGui::TextDisabled(">");
        ImGui::SameLine(0, 2);
        ImGui::PushID(walked.c_str());
        if (ImGui::Button(part.c_str())) { m_Folder = walked; m_ForceScan = true; }
        ImGui::PopID();
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    ImGui::PopStyleColor();

    const float rightWidth = 360.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowContentRegionMax().x - rightWidth));
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::InputTextWithHint("##assetsearch", "Search all assets", m_Search, sizeof(m_Search))) m_ForceScan = true;
    ImGui::SameLine();
    if (ImGui::Button(m_ListView ? "Tiles" : "List")) m_ListView = !m_ListView;
    ImGui::SetItemTooltip(m_ListView ? "Show as tiles" : "Show as a list");
    if (!m_ListView) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##tilesize", &m_TileSize, 56.0f, 140.0f, "Size");
    }

    // ---- folder tree (left) and the folder's contents (right)
    if (ImGui::BeginTable("##assetsplit", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
        ImGui::TableSetupColumn("folders", ImGuiTableColumnFlags_WidthFixed, 190.0f);
        ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##foldertree");
        DrawFolderTree("", 0);
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##files");
        if (m_Entries.empty())
            ImGui::TextDisabled(m_Search[0] ? "Nothing matches \"%s\"" : "This folder is empty. Right-click to create a script or a folder.", m_Search);

        if (m_ListView) {
            if (ImGui::BeginTable("##filelist", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();
                for (const auto& e : m_Entries) DrawListRow(e);
                ImGui::EndTable();
            }
        }
        else {
            const float spacing = 8.0f;
            const float avail = ImGui::GetContentRegionAvail().x;
            const int columns = std::max(1, (int)((avail + spacing) / (m_TileSize + spacing)));
            for (size_t i = 0; i < m_Entries.size(); i++) {
                if (i % columns != 0) ImGui::SameLine(0, spacing);
                DrawTile(m_Entries[i], m_TileSize, thumbnail);
            }
        }

        // Right-click on empty space: create things in this folder
        if (ImGui::BeginPopupContextWindow("##filesempty", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("New Script")) {
                const std::string name = UniqueName(m_Folder, "NewScript", ".lua");
                const std::string rel = (m_Folder.empty() ? "" : m_Folder + "/") + name;
                std::ofstream out(fs::u8path(m_AssetDir + rel));
                char text[1024];
                std::snprintf(text, sizeof(text), kScriptTemplate, name.c_str());
                out << text;
                out.close();
                m_Selected = rel;
                m_RenameTarget = rel;                  // name it right away, like other engines
                std::snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", name.c_str());
                m_OpenRename = true;
                m_ForceScan = true;
                m_Status = "Created " + rel;
            }
            if (ImGui::MenuItem("New Folder")) {
                const std::string name = UniqueName(m_Folder, "New Folder", "");
                const std::string rel = (m_Folder.empty() ? "" : m_Folder + "/") + name;
                std::error_code ec;
                fs::create_directory(fs::u8path(m_AssetDir + rel), ec);
                m_Selected = rel;
                m_RenameTarget = rel;
                std::snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", name.c_str());
                m_OpenRename = true;
                m_ForceScan = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Show in Explorer")) Platform::ShowInExplorer(m_AssetDir + m_Folder);
            if (ImGui::MenuItem("Refresh")) m_ForceScan = true;
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }

    // ---- footer: what is selected, or the last file operation
    if (!m_Status.empty()) ImGui::TextDisabled("%s", m_Status.c_str());
    else if (!m_Selected.empty()) ImGui::TextDisabled("%s", m_Selected.c_str());
    else ImGui::TextDisabled("%zu items. Drag files into the scene, the Hierarchy or Inspector fields.", m_Entries.size());

    DrawDialogs();
    ImGui::End();
    return m_Actions;
}

void AssetBrowser::DrawFolderTree(const std::string& folder, int depth) {
    std::vector<std::string> children;
    for (const auto& f : m_Folders) if (FolderOf(f) == folder) children.push_back(f);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (folder.empty()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    if (folder == m_Folder && m_Search[0] == '\0') flags |= ImGuiTreeNodeFlags_Selected;

    const bool open = ImGui::TreeNodeEx(folder.empty() ? "##root" : folder.c_str(), flags, "%s",
        folder.empty() ? "Assets" : NameOf(folder).c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { m_Folder = folder; m_Search[0] = '\0'; m_ForceScan = true; }
    if (open) {
        if (depth < 16) for (const auto& c : children) DrawFolderTree(c, depth + 1);
        ImGui::TreePop();
    }
}

void AssetBrowser::DrawIcon(ImDrawList* dl, const ImVec2& mn, const ImVec2& mx, const Entry& e,
    const std::function<uint32_t(const std::string&)>& thumbnail) {
    const float w = mx.x - mn.x, h = mx.y - mn.y;
    if (e.Type == Kind::Folder) {
        const ImU32 back = IM_COL32(196, 160, 72, 255), front = IM_COL32(226, 188, 92, 255);
        dl->AddRectFilled(ImVec2(mn.x, mn.y + h * 0.12f), ImVec2(mn.x + w * 0.45f, mn.y + h * 0.3f), back, 3.0f);
        dl->AddRectFilled(ImVec2(mn.x, mn.y + h * 0.22f), mx, back, 4.0f);
        dl->AddRectFilled(ImVec2(mn.x, mn.y + h * 0.32f), mx, front, 4.0f);
        return;
    }
    if (e.Type == Kind::Texture) {
        if (const uint32_t tex = thumbnail ? thumbnail(e.Path) : 0) {
            dl->AddRectFilled(mn, mx, IM_COL32(40, 42, 48, 255), 3.0f);    // shows through transparent pixels
            dl->AddImage((ImTextureID)(intptr_t)tex, mn, mx, ImVec2(0, 1), ImVec2(1, 0));
            dl->AddRect(mn, mx, IM_COL32(0, 0, 0, 120), 3.0f);
            return;
        }
    }
    // A document with a folded corner and a coloured type badge
    const KindStyle st = StyleOf(e.Type, e.Name);
    const float fold = w * 0.22f;
    const ImU32 paper = IM_COL32(58, 61, 69, 255);
    dl->AddRectFilled(mn, ImVec2(mx.x - fold, mx.y), paper, 4.0f, ImDrawFlags_RoundCornersLeft | ImDrawFlags_RoundCornersBottomRight);
    dl->AddRectFilled(ImVec2(mx.x - fold, mn.y + fold), mx, paper, 4.0f, ImDrawFlags_RoundCornersBottomRight);
    dl->AddTriangleFilled(ImVec2(mx.x - fold, mn.y), ImVec2(mx.x, mn.y + fold), ImVec2(mx.x - fold, mn.y + fold), IM_COL32(88, 92, 102, 255));
    if (e.Type == Kind::Environment) {                     // a little sky
        dl->AddRectFilledMultiColor(ImVec2(mn.x + w * 0.12f, mn.y + h * 0.18f), ImVec2(mx.x - w * 0.12f, mn.y + h * 0.55f),
            IM_COL32(70, 120, 200, 255), IM_COL32(70, 120, 200, 255), IM_COL32(200, 215, 235, 255), IM_COL32(200, 215, 235, 255));
    }
    const ImVec2 ts = ImGui::CalcTextSize(st.Label);
    const float bw = std::min(w * 0.9f, ts.x + 10.0f), bh = ts.y + 4.0f;
    const ImVec2 bmn(mn.x + (w - fold * 0.5f - bw) * 0.5f, mn.y + h * 0.62f);
    dl->AddRectFilled(bmn, ImVec2(bmn.x + bw, bmn.y + bh), st.Color, 3.0f);
    dl->PushClipRect(bmn, ImVec2(bmn.x + bw, bmn.y + bh), true);
    dl->AddText(ImVec2(bmn.x + (bw - ts.x) * 0.5f, bmn.y + 2.0f), IM_COL32(255, 255, 255, 240), st.Label);
    dl->PopClipRect();
}

void AssetBrowser::DrawTile(const Entry& e, float size, const std::function<uint32_t(const std::string&)>& thumbnail) {
    ImGui::PushID(e.Path.c_str());
    const float textH = ImGui::GetTextLineHeight();
    const ImVec2 tile(size, size + textH + 6.0f);
    ImGui::InvisibleButton("##tile", tile);
    const ImVec2 mn = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool selected = e.Path == m_Selected, hovered = ImGui::IsItemHovered();
    if (selected || hovered)
        dl->AddRectFilled(mn, ImVec2(mn.x + tile.x, mn.y + tile.y), selected ? IM_COL32(56, 92, 150, 200) : IM_COL32(255, 255, 255, 18), 4.0f);
    const float pad = size * 0.14f;
    DrawIcon(dl, ImVec2(mn.x + pad, mn.y + pad * 0.7f), ImVec2(mn.x + size - pad, mn.y + size - pad * 0.6f), e, thumbnail);

    // The name under it, shortened with "..." to fit
    std::string label = e.Name;
    const float maxW = size - 4.0f;
    if (ImGui::CalcTextSize(label.c_str()).x > maxW) {
        while (label.size() > 1 && ImGui::CalcTextSize((label + "...").c_str()).x > maxW) label.pop_back();
        label += "...";
    }
    const float lw = ImGui::CalcTextSize(label.c_str()).x;
    dl->AddText(ImVec2(mn.x + (size - lw) * 0.5f, mn.y + size + 1.0f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());

    HandleItem(e);
    ImGui::PopID();
}

void AssetBrowser::DrawListRow(const Entry& e) {
    ImGui::PushID(e.Path.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 iconPos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    DrawIcon(ImGui::GetWindowDrawList(), iconPos, ImVec2(iconPos.x + h, iconPos.y + h), e, nullptr);
    ImGui::SameLine();
    ImGui::Selectable(e.Name.c_str(), e.Path == m_Selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
    HandleItem(e);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextDisabled("%s", KindName(e.Type));
    ImGui::TableSetColumnIndex(2);
    if (e.Type != Kind::Folder) {
        if (e.Size >= 1024 * 1024) ImGui::TextDisabled("%.1f MB", e.Size / (1024.0 * 1024.0));
        else ImGui::TextDisabled("%.1f KB", e.Size / 1024.0);
    }
    ImGui::PopID();
}

void AssetBrowser::HandleItem(const Entry& e) {
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) { m_Selected = e.Path; m_Status.clear(); }

    auto open = [&]() {
        switch (e.Type) {
        case Kind::Folder: m_Folder = e.Path; m_Search[0] = '\0'; m_ForceScan = true; break;
        case Kind::Scene: m_Actions.push_back({ Action::OpenScene, e.Path }); break;
        case Kind::Model: case Kind::Prefab: m_Actions.push_back({ Action::Place, e.Path }); break;
        case Kind::Sound: m_Actions.push_back({ Action::PlaySound, e.Path }); break;
        case Kind::Environment: m_Actions.push_back({ Action::SetSky, e.Path }); break;
        case Kind::Dialogue: m_Actions.push_back({ Action::OpenDialogue, e.Path }); break;
        default: m_Actions.push_back({ Action::OpenExternal, e.Path }); break;
        }
    };
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) open();

    if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        ImGui::SetItemTooltip("%s\n%s", e.Path.c_str(), KindName(e.Type));

    // Drag a file anywhere that takes assets (viewport, Hierarchy, Inspector fields)
    if (e.Type != Kind::Folder && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(Payload, e.Path.c_str(), e.Path.size() + 1);
        ImGui::Text("%s", e.Name.c_str());
        ImGui::TextDisabled("%s", KindName(e.Type));
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginPopupContextItem("##item")) {
        m_Selected = e.Path;
        const char* openLabel = e.Type == Kind::Folder ? "Open" : e.Type == Kind::Scene ? "Open Scene"
            : (e.Type == Kind::Model || e.Type == Kind::Prefab) ? "Place in Scene" : e.Type == Kind::Sound ? "Play"
            : e.Type == Kind::Environment ? "Use as Sky" : e.Type == Kind::Dialogue ? "Edit Dialogue" : "Open";
        if (ImGui::MenuItem(openLabel)) open();
        if (e.Type != Kind::Folder && ImGui::MenuItem("Open with System")) m_Actions.push_back({ Action::OpenExternal, e.Path });
        if (ImGui::MenuItem("Show in Explorer")) Platform::ShowInExplorer(m_AssetDir + e.Path);
        if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(e.Path.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Rename...")) {
            m_RenameTarget = e.Path;
            std::snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", e.Name.c_str());
            m_OpenRename = true;
        }
        if (ImGui::MenuItem("Delete...")) { m_DeleteTarget = e.Path; m_OpenDelete = true; }
        ImGui::EndPopup();
    }
}

void AssetBrowser::DrawDialogs() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    if (m_OpenRename) { ImGui::OpenPopup("Rename Asset"); m_OpenRename = false; }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Rename %s", m_RenameTarget.c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(300.0f);
        const bool enter = ImGui::InputText("##renameto", m_RenameBuf, sizeof(m_RenameBuf),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::TextDisabled("Scenes and scripts that use the old name are not updated.");
        if (ImGui::Button("Rename", ImVec2(120, 0)) || enter) {
            std::string name = m_RenameBuf;
            const fs::path from = fs::u8path(m_AssetDir + m_RenameTarget);
            // Keep the extension if it was left out
            if (!fs::is_directory(from) && fs::u8path(name).extension().empty()) name += U8(from.extension().u8string());
            const std::string folder = FolderOf(m_RenameTarget);
            const std::string rel = (folder.empty() ? "" : folder + "/") + name;
            const fs::path to = fs::u8path(m_AssetDir + rel);
            std::error_code ec;
            if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos) m_Status = "Rename: not a valid file name";
            else if (rel != m_RenameTarget && fs::exists(to)) m_Status = "Rename: " + rel + " already exists";
            else if (rel != m_RenameTarget) {
                fs::rename(from, to, ec);
                m_Status = ec ? "Rename failed: " + ec.message() : "Renamed to " + rel;
                if (!ec) { if (m_Selected == m_RenameTarget) m_Selected = rel; if (m_Folder == m_RenameTarget) m_Folder = rel; }
            }
            m_ForceScan = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_OpenDelete) { ImGui::OpenPopup("Delete Asset"); m_OpenDelete = false; }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Move %s to the Recycle Bin?", m_DeleteTarget.c_str());
        ImGui::TextDisabled("Scenes and scripts that use it will not find it any more.");
        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            const bool ok = Platform::MoveToRecycleBin(m_AssetDir + m_DeleteTarget);
            m_Status = ok ? "Moved " + m_DeleteTarget + " to the Recycle Bin" : "Could not delete " + m_DeleteTarget;
            if (ok && m_Selected == m_DeleteTarget) m_Selected.clear();
            if (ok && (m_Folder == m_DeleteTarget || m_Folder.rfind(m_DeleteTarget + "/", 0) == 0)) m_Folder = FolderOf(m_DeleteTarget);
            m_ForceScan = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
