#include "DatabaseEditor.h"
#include "AssetBrowser.h"

#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

using namespace Engine;
using json = nlohmann::json;

namespace {
    int ResizeCallback(ImGuiInputTextCallbackData* d) {
        if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* s = static_cast<std::string*>(d->UserData);
            s->resize(d->BufTextLen);
            d->Buf = s->data();
        }
        return 0;
    }

    constexpr int KindBit(AssetBrowser::Kind k) { return 1 << (int)k; }
    constexpr int kModels = KindBit(AssetBrowser::Kind::Model) | KindBit(AssetBrowser::Kind::Prefab);

    bool Contains(const std::string& text, const char* search) {
        if (!search[0]) return true;
        std::string a = text, b = search;
        for (auto& c : a) c = (char)std::tolower((unsigned char)c);
        for (auto& c : b) c = (char)std::tolower((unsigned char)c);
        return a.find(b) != std::string::npos;
    }

    std::string Slug(const std::string& name) {
        std::string s;
        for (char c : name) {
            if (std::isalnum((unsigned char)c)) s += (char)std::tolower((unsigned char)c);
            else if ((c == ' ' || c == '_' || c == '-') && !s.empty() && s.back() != '_') s += '_';
        }
        while (!s.empty() && s.back() == '_') s.pop_back();
        return s;
    }

    // "On use: +10 health", "While equipped: +2 armour", "On hit: -1 health per second for 5 s"
    std::string EffectSummary(const ItemEffect& e) {
        const char* when = e.Trigger == EffectTrigger::Equip ? "While equipped" : e.Trigger == EffectTrigger::Hit ? "On hit" : "On use";
        if (e.Trigger == EffectTrigger::Equip && (e.Kind == EffectKind::Add || e.Kind == EffectKind::Event)) when = "When equipped";
        char buf[160];
        const char* sign = e.Amount >= 0.0f ? "+" : "";
        switch (e.Kind) {
        case EffectKind::Add: std::snprintf(buf, sizeof(buf), "%s: %s%g %s", when, sign, e.Amount, e.Stat.c_str()); break;
        case EffectKind::Modifier:
            if (e.Duration > 0.0f && e.Trigger != EffectTrigger::Equip)
                std::snprintf(buf, sizeof(buf), "%s: %s%g %s for %g s", when, sign, e.Amount, e.Stat.c_str(), e.Duration);
            else std::snprintf(buf, sizeof(buf), "%s: %s%g %s", when, sign, e.Amount, e.Stat.c_str());
            break;
        case EffectKind::OverTime:
            std::snprintf(buf, sizeof(buf), "%s: %s%g %s per second%s", when, sign, e.Amount, e.Stat.c_str(),
                          e.Duration > 0.0f ? (" for " + std::to_string((int)std::lround(e.Duration)) + " s").c_str() : "");
            break;
        case EffectKind::Event: std::snprintf(buf, sizeof(buf), "%s: event \"%s\"", when, e.Event.c_str()); break;
        }
        return buf;
    }

    // A description as the game shows it: the UI markup of UILayout (ParseMarkup) applied, word-wrapped to the
    // width. {#rrggbb} colour, {/} back to the default colour, {b}..{/b} bold (drawn twice, 1 px apart);
    // anything else in braces stays as text.
    void MarkupPreview(const std::string& text) {
        const float width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().WindowPadding.x * 2.0f;
        if (!ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY)) { ImGui::EndChild(); return; }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float line = ImGui::GetTextLineHeight(), space = ImGui::CalcTextSize(" ").x;
        const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Text);
        ImU32 color = normal;
        bool bold = false, gap = false;
        float x = 0.0f, y = 0.0f;
        std::string word;
        auto flush = [&]() {
            if (word.empty()) return;
            const float w = ImGui::CalcTextSize(word.c_str()).x + (bold ? 1.0f : 0.0f);
            const float at = x + (gap && x > 0.0f ? space : 0.0f);
            if (at > 0.0f && at + w > width) { x = 0.0f; y += line; }
            else x = at;
            const ImVec2 p(origin.x + x, origin.y + y);
            dl->AddText(p, color, word.c_str());
            if (bold) dl->AddText(ImVec2(p.x + 1.0f, p.y), color, word.c_str());
            x += w;
            word.clear();
            gap = false;
        };
        for (size_t i = 0; i < text.size(); i++) {
            const char c = text[i];
            if (c == ' ' || c == '\t') { flush(); gap = true; continue; }
            if (c == '\n') { flush(); x = 0.0f; y += line; gap = false; continue; }
            if (c == '{') {
                const size_t end = text.find('}', i);
                if (end != std::string::npos && end - i <= 8) {
                    const std::string tag = text.substr(i + 1, end - i - 1);
                    unsigned rgb = 0;
                    bool known = true;
                    if (tag == "/") { flush(); color = normal; }
                    else if (tag == "b") { flush(); bold = true; }
                    else if (tag == "/b") { flush(); bold = false; }
                    else if (tag.size() == 7 && tag[0] == '#' && std::sscanf(tag.c_str() + 1, "%6x", &rgb) == 1) {
                        flush();
                        color = IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
                    }
                    else known = false;
                    if (known) {
                        i = end;
                        continue;
                    }
                }
            }
            word += c;
        }
        flush();
        if (text.empty()) dl->AddText(origin, ImGui::GetColorU32(ImGuiCol_TextDisabled), "(no description)");
        ImGui::Dummy(ImVec2(width, y + line));
        ImGui::EndChild();
    }

    const std::vector<const char*> kItemKinds ={ "weapon", "armour", "consumable", "treasure", "quest", "material", "misc" };
    const std::vector<const char*> kSlots = { "head", "body", "hands", "feet", "weapon", "shield", "neck", "ring" };
}

DatabaseEditor::DatabaseEditor(std::string assetDir) : m_AssetDir(std::move(assetDir)) {}

void DatabaseEditor::EnsureLoaded() {
    if (m_Loaded) return;
    m_Loaded = true;
    const std::string file = m_AssetDir + GameDatabase::kPath;
    std::error_code ec;
    m_Db = GameDatabase();
    if (std::filesystem::exists(std::filesystem::u8path(file), ec)) {
        std::string error;
        if (auto db = GameDatabase::Load(file, &error)) m_Db = *db;
        else m_Status = "Can't read " + std::string(GameDatabase::kPath) + ": " + error;
        m_SavedText = m_Db.ToJson().dump();
    }
    else {
        // a new database starts with a health stat (what kills an NPC) and its ceiling
        m_Db.Stats.push_back({ "health", "Health", "Hit points: the NPC dies at 0", 20.0f, 0.0f, 9999.0f, "max_health" });
        m_Db.Stats.push_back({ "max_health", "Max health", "", 20.0f, 1.0f, 9999.0f, "" });
        m_SavedText.clear();                     // not on disk yet
    }
    m_History.clear();
    m_HistoryPos = -1;
    Commit();
    m_PublishedText.clear();
    Publish();
}

bool DatabaseEditor::IsDirty() const {
    if (!m_Loaded) return false;
    return m_Db.ToJson().dump() != m_SavedText && !(m_SavedText.empty() && m_HistoryPos <= 0);
}

bool DatabaseEditor::Save() {
    EnsureLoaded();
    const std::string file = m_AssetDir + GameDatabase::kPath;
    if (!m_Db.Save(file)) { m_Status = "Can't write " + std::string(GameDatabase::kPath); return false; }
    m_SavedText = m_Db.ToJson().dump();
    m_Status = "Saved " + std::string(GameDatabase::kPath);
    return true;
}

void DatabaseEditor::Reload() {
    m_Loaded = false;
    EnsureLoaded();
    m_Status = "Reloaded from the file";
}

void DatabaseEditor::Commit() {
    const std::string s = m_Db.ToJson().dump();
    if (m_HistoryPos >= 0 && m_History[m_HistoryPos] == s) return;
    m_History.resize(m_HistoryPos + 1);
    m_History.push_back(s);
    if (m_History.size() > 200) m_History.erase(m_History.begin());
    m_HistoryPos = (int)m_History.size() - 1;
}

void DatabaseEditor::Undo() {
    Commit();
    if (m_HistoryPos <= 0) return;
    GameDatabase::FromJson(json::parse(m_History[--m_HistoryPos]), m_Db);
    m_IdEditOwner = nullptr;
}

void DatabaseEditor::Redo() {
    if (m_HistoryPos + 1 >= (int)m_History.size()) return;
    GameDatabase::FromJson(json::parse(m_History[++m_HistoryPos]), m_Db);
    m_IdEditOwner = nullptr;
}

void DatabaseEditor::Publish() {
    const std::string s = m_Db.ToJson().dump();
    if (s == m_PublishedText) return;
    m_PublishedText = s;
    if (OnChanged) OnChanged(std::make_shared<const GameDatabase>(m_Db));
}

void DatabaseEditor::SelectNpc(const std::string& id) {
    EnsureLoaded();
    for (int i = 0; i < (int)m_Db.Npcs.size(); i++) if (m_Db.Npcs[i].Id == id) m_SelNpc = i;
    m_ForceTab = Tab::Npcs;
    m_ForceTabNext = true;
}

void DatabaseEditor::SelectItem(const std::string& id) {
    EnsureLoaded();
    for (int i = 0; i < (int)m_Db.Items.size(); i++) if (m_Db.Items[i].Id == id) m_SelItem = i;
    m_ForceTab = Tab::Items;
    m_ForceTabNext = true;
}

int& DatabaseEditor::Selected() { return m_Tab == Tab::Items ? m_SelItem : m_Tab == Tab::Npcs ? m_SelNpc : m_SelStat; }
int DatabaseEditor::Count() const {
    return (int)(m_Tab == Tab::Items ? m_Db.Items.size() : m_Tab == Tab::Npcs ? m_Db.Npcs.size() : m_Db.Stats.size());
}

template<typename T>
std::string DatabaseEditor::UniqueId(const std::vector<T>& list, const std::string& base) const {
    const std::string b = base.empty() ? "entry" : base;
    for (int n = 1; ; n++) {
        const std::string id = n == 1 ? b : b + "_" + std::to_string(n);
        if (std::none_of(list.begin(), list.end(), [&](const T& e) { return e.Id == id; })) return id;
    }
}

void DatabaseEditor::NewEntry() {
    if (m_Tab == Tab::Items) {
        ItemDef d;
        d.Id = UniqueId(m_Db.Items, "item");
        d.Name = "New item";
        m_Db.Items.push_back(d);
        m_SelItem = (int)m_Db.Items.size() - 1;
    }
    else if (m_Tab == Tab::Npcs) {
        NpcDef d;
        d.Id = UniqueId(m_Db.Npcs, "npc");
        d.Name = "New NPC";
        m_Db.Npcs.push_back(d);
        m_SelNpc = (int)m_Db.Npcs.size() - 1;
    }
    else {
        StatDef d;
        d.Id = UniqueId(m_Db.Stats, "stat");
        d.Name = "New stat";
        m_Db.Stats.push_back(d);
        m_SelStat = (int)m_Db.Stats.size() - 1;
    }
    m_IdEditOwner = nullptr;
}

void DatabaseEditor::DuplicateEntry(int i) {
    if (i < 0 || i >= Count()) return;
    if (m_Tab == Tab::Items) {
        ItemDef d = m_Db.Items[i];
        d.Id = UniqueId(m_Db.Items, d.Id);
        d.Name += " (copy)";
        m_Db.Items.insert(m_Db.Items.begin() + i + 1, d);
    }
    else if (m_Tab == Tab::Npcs) {
        NpcDef d = m_Db.Npcs[i];
        d.Id = UniqueId(m_Db.Npcs, d.Id);
        d.Name += " (copy)";
        m_Db.Npcs.insert(m_Db.Npcs.begin() + i + 1, d);
    }
    else {
        StatDef d = m_Db.Stats[i];
        d.Id = UniqueId(m_Db.Stats, d.Id);
        d.Name += " (copy)";
        m_Db.Stats.insert(m_Db.Stats.begin() + i + 1, d);
    }
    Selected() = i + 1;
    m_IdEditOwner = nullptr;
}

void DatabaseEditor::DeleteEntry(int i) {
    if (i < 0 || i >= Count()) return;
    if (m_Tab == Tab::Items) m_Db.Items.erase(m_Db.Items.begin() + i);
    else if (m_Tab == Tab::Npcs) m_Db.Npcs.erase(m_Db.Npcs.begin() + i);
    else m_Db.Stats.erase(m_Db.Stats.begin() + i);
    Selected() = std::min(i, Count() - 1);
    m_IdEditOwner = nullptr;
}

void DatabaseEditor::RenameItem(const std::string& from, const std::string& to) {
    for (auto& n : m_Db.Npcs) for (auto& e : n.Inventory) if (e.Item == from) e.Item = to;
}

void DatabaseEditor::RenameStat(const std::string& from, const std::string& to) {
    if (m_Db.DeathStat == from) m_Db.DeathStat = to;
    for (auto& s : m_Db.Stats) if (s.MaxStat == from) s.MaxStat = to;
    for (auto& it : m_Db.Items) for (auto& e : it.Effects) if (e.Stat == from) e.Stat = to;
    for (auto& n : m_Db.Npcs)
        if (auto f = n.Stats.find(from); f != n.Stats.end()) { const float v = f->second; n.Stats.erase(f); n.Stats[to] = v; }
}

// ---------------------------------------------------------------- widgets

bool DatabaseEditor::Text(const char* label, std::string& s, bool multiline) {
    if (multiline)
        return ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.5f),
                                         ImGuiInputTextFlags_CallbackResize, ResizeCallback, &s);
    return ImGui::InputText(label, s.data(), s.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeCallback, &s);
}

bool DatabaseEditor::IdField(const char* what, std::string& id) {
    if (m_IdEditOwner != &id) { m_IdEdit = id; m_IdEditOwner = &id; }
    ImGui::InputText("Id", m_IdEdit.data(), m_IdEdit.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeCallback, &m_IdEdit);
    ImGui::SetItemTooltip("What scripts and other entries use to find it (db.%s(\"%s\")). Letters, digits and _.\n"
                          "Renaming updates the references in the database; scenes keep the old id.", what, id.c_str());
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    const std::string to = Slug(m_IdEdit);
    if (to.empty() || to == id) { m_IdEdit = id; return false; }
    bool taken = false;
    if (m_Tab == Tab::Items) taken = m_Db.Item(to) != nullptr;
    else if (m_Tab == Tab::Npcs) taken = m_Db.Npc(to) != nullptr;
    else taken = m_Db.Stat(to) != nullptr;
    if (taken) { m_Status = "The id \"" + to + "\" is taken"; m_IdEdit = id; return false; }
    const std::string from = id;
    id = to;
    if (m_Tab == Tab::Items) RenameItem(from, to);
    else if (m_Tab == Tab::Stats) RenameStat(from, to);
    m_IdEdit = to;
    m_Status = "Renamed " + from + " to " + to;
    return true;
}

bool DatabaseEditor::AssetCombo(const char* label, std::string& path, const std::vector<const char*>& folders, int kinds) {
    bool changed = false;
    std::error_code ec;
    const bool missing = !path.empty() && !std::filesystem::exists(std::filesystem::u8path(m_AssetDir + path), ec);
    const std::string preview = path.empty() ? "(none)" : missing ? path + " (missing)" : path;
    if (ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("(none)", path.empty())) { path.clear(); changed = true; }
        const auto root = std::filesystem::u8path(m_AssetDir);
        for (const char* folder : folders)
            for (const auto& f : std::filesystem::recursive_directory_iterator(root / folder, ec)) {
                if (!f.is_regular_file()) continue;
                const std::string rel = std::filesystem::relative(f.path(), root, ec).generic_string();
                if (!(kinds & KindBit(AssetBrowser::KindOf(rel)))) continue;
                if (ImGui::Selectable(rel.c_str(), rel == path)) { path = rel; changed = true; }
            }
        ImGui::EndCombo();
    }
    // dragged from the Assets panel
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(AssetBrowser::Payload)) {
            const std::string rel = AssetBrowser::PayloadPath(p);
            if (kinds & KindBit(AssetBrowser::KindOf(rel))) { path = rel; changed = true; }
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

bool DatabaseEditor::StatCombo(const char* label, std::string& stat, const char* none) {
    bool changed = false;
    const StatDef* d = m_Db.Stat(stat);
    const std::string preview = stat.empty() ? (none ? none : "(pick a stat)") : d ? d->Name + "  (" + stat + ")" : stat + " (not a stat)";
    if (ImGui::BeginCombo(label, preview.c_str())) {
        if (none && ImGui::Selectable(none, stat.empty())) { stat.clear(); changed = true; }
        for (const auto& s : m_Db.Stats)
            if (ImGui::Selectable((s.Name + "  (" + s.Id + ")").c_str(), s.Id == stat)) { stat = s.Id; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

bool DatabaseEditor::ItemCombo(const char* label, std::string& item) {
    bool changed = false;
    const ItemDef* d = m_Db.Item(item);
    const std::string preview = item.empty() ? "(pick an item)" : d ? d->Name : item + " (not an item)";
    if (ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (const auto& it : m_Db.Items) {
            ImGui::PushID(it.Id.c_str());
            Icon(it.Icon, ImGui::GetTextLineHeight());
            ImGui::SameLine();
            if (ImGui::Selectable((it.Name + "  (" + it.Id + ")").c_str(), it.Id == item)) { item = it.Id; changed = true; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(ItemPayload)) {
            item.assign((const char*)p->Data, p->DataSize);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

// A text field with a list of usual values beside it
bool DatabaseEditor::PresetText(const char* label, std::string& s, const std::vector<const char*>& presets) {
    ImGui::PushID(label);
    const float button = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - button - ImGui::GetStyle().ItemInnerSpacing.x);
    bool changed = Text("##text", s);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::BeginCombo("##presets", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
        for (const char* p : presets) if (ImGui::Selectable(p, s == p)) { s = p; changed = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

void DatabaseEditor::Icon(const std::string& path, float size) {
    const uint32_t tex = !path.empty() && TextureId ? TextureId(path) : 0;
    if (tex) ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(size, size), ImVec2(0, 1), ImVec2(1, 0));
    else ImGui::Dummy(ImVec2(size, size));
}

// ---------------------------------------------------------------- window

void DatabaseEditor::Draw(bool* open) {
    EnsureLoaded();
    const std::string title = std::string("Database") + (IsDirty() ? " *" : "") + "###DatabaseEditor";
    ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), open, ImGuiWindowFlags_MenuBar)) { m_Focused = false; ImGui::End(); return; }
    m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // shortcuts while the window has focus (the scene's are off then)
    const ImGuiIO& io = ImGui::GetIO();
    if (m_Focused) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
        if (!io.WantTextInput) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) Redo(); else Undo(); }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) DuplicateEntry(Selected());
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteEntry(Selected());
        }
    }

    DrawMenuBar();

    if (ImGui::BeginTabBar("##tabs")) {
        auto tab = [&](const char* label, Tab t) {
            const ImGuiTabItemFlags f = m_ForceTabNext && m_ForceTab == t ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label, nullptr, f)) {
                if (m_Tab != t) m_IdEditOwner = nullptr;
                m_Tab = t;
                ImGui::EndTabItem();
            }
        };
        tab("Items", Tab::Items);
        tab("NPCs", Tab::Npcs);
        tab("Stats", Tab::Stats);
        m_ForceTabNext = false;
        ImGui::EndTabBar();
    }

    const float footer = ImGui::GetFrameHeightWithSpacing();
    const float listW = std::clamp(ImGui::GetContentRegionAvail().x * 0.28f, 220.0f, 320.0f);
    ImGui::BeginChild("##list", ImVec2(listW, -footer), ImGuiChildFlags_Border);
    DrawList();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##props", ImVec2(0, -footer), ImGuiChildFlags_Border);
    if (Selected() < 0 && Count() > 0) Selected() = 0;
    const int sel = Selected();
    ImGui::PushItemWidth(-160.0f);
    if (sel >= 0 && sel < Count()) {
        if (m_Tab == Tab::Items) DrawItem(m_Db.Items[sel]);
        else if (m_Tab == Tab::Npcs) DrawNpc(m_Db.Npcs[sel]);
        else DrawStat(m_Db.Stats[sel]);
    }
    else ImGui::TextDisabled("Nothing selected. + New adds one.");
    ImGui::PopItemWidth();
    ImGui::EndChild();
    DrawProblems();

    // one undo step per finished interaction, and every change goes live at once
    if (!ImGui::IsAnyItemActive()) Commit();
    Publish();
    ImGui::End();
}

void DatabaseEditor::DrawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save", "Ctrl+S")) Save();
        if (ImGui::MenuItem("Reload from file", nullptr, false, IsDirty())) Reload();
        ImGui::SetItemTooltip("Throw away the unsaved changes");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_HistoryPos > 0)) Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_HistoryPos + 1 < (int)m_History.size())) Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("New", nullptr)) NewEntry();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, Selected() >= 0)) DuplicateEntry(Selected());
        if (ImGui::MenuItem("Delete", "Delete", false, Selected() >= 0)) DeleteEntry(Selected());
        ImGui::EndMenu();
    }
    ImGui::TextDisabled("%s", GameDatabase::kPath);
    ImGui::EndMenuBar();
}

void DatabaseEditor::DrawList() {
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("+ New").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##search", "Search", m_Search, sizeof(m_Search));
    ImGui::SameLine();
    if (ImGui::Button("+ New")) NewEntry();
    ImGui::Separator();

    int duplicate = -1, remove = -1;
    const float icon = ImGui::GetTextLineHeightWithSpacing() * 2.0f;   // two lines: name and id
    for (int i = 0; i < Count(); i++) {
        std::string id, name, iconPath, extra;
        if (m_Tab == Tab::Items) { const auto& d = m_Db.Items[i]; id = d.Id; name = d.Name; iconPath = d.Icon; extra = d.Kind; }
        else if (m_Tab == Tab::Npcs) { const auto& d = m_Db.Npcs[i]; id = d.Id; name = d.Name; }
        else { const auto& d = m_Db.Stats[i]; id = d.Id; name = d.Name; }
        if (!Contains(name, m_Search) && !Contains(id, m_Search) && !Contains(extra, m_Search)) continue;
        ImGui::PushID(i);
        const bool selected = Selected() == i;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float h = icon;
        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, h))) {
            if (Selected() != i) m_IdEditOwner = nullptr;
            Selected() = i;
        }
        // drag an NPC or item into the Viewport to place it (or an item onto an inventory line)
        if (m_Tab != Tab::Stats && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(m_Tab == Tab::Npcs ? NpcPayload : ItemPayload, id.data(), id.size());
            ImGui::Text("%s", name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("##row menu")) {
            Selected() = i;
            if (ImGui::MenuItem("Duplicate")) duplicate = i;
            if (ImGui::MenuItem("Delete")) remove = i;
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(start);
        if (m_Tab == Tab::Items) { Icon(iconPath, icon); ImGui::SameLine(); }
        ImGui::BeginGroup();
        ImGui::TextUnformatted(name.empty() ? "(no name)" : name.c_str());
        ImGui::TextDisabled("%s%s%s", id.c_str(), extra.empty() ? "" : "  ", extra.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (duplicate >= 0) DuplicateEntry(duplicate);
    if (remove >= 0) DeleteEntry(remove);

    if (m_Tab == Tab::Stats) {
        ImGui::Separator();
        ImGui::SetNextItemWidth(-FLT_MIN);
        StatCombo("##death", m_Db.DeathStat, "(none: NPCs die only by kill())");
        ImGui::SetItemTooltip("The death stat: an NPC dies when it reaches its minimum (npc:damage lowers it)");
    }
}

void DatabaseEditor::DrawItem(ItemDef& it) {
    ImGui::PushID(&it);
    Icon(it.Icon, 64.0f);
    ImGui::SameLine();
    ImGui::BeginGroup();
    IdField("item", it.Id);
    Text("Name", it.Name);
    PresetText("Kind", it.Kind, kItemKinds);
    PresetText("Slot", it.Slot, kSlots);
    ImGui::SetItemTooltip("Where it is equipped. Empty: it can't be equipped.");
    ImGui::EndGroup();

    ImGui::SeparatorText("Looks");
    AssetCombo("Icon", it.Icon, { "Textures" }, KindBit(AssetBrowser::Kind::Texture));
    ImGui::SetItemTooltip("Shown in inventories (drop an image from Assets here)");
    AssetCombo("Model", it.Model, { "Models", "Prefabs" }, kModels);
    ImGui::SetItemTooltip("How it looks lying on the ground (a dropped pickup). None: a small gold box.");
    if (!it.Model.empty()) ImGui::DragFloat("Model height", &it.ModelHeight, 0.01f, 0.02f, 20.0f, "%.2f m");
    ImGui::Text("Description");
    Text("##description", it.Description, true);
    ImGui::SetItemTooltip("Markup: {#ffcc66}coloured text{/}  {b}bold text{/b}  (a new line: Enter)");
    ImGui::TextDisabled("As the game shows it:");
    MarkupPreview(it.Description);

    ImGui::SeparatorText("Rules");
    ImGui::DragInt("Value", &it.Value, 1.0f, 0, 1000000);
    ImGui::Checkbox("Stackable", &it.Stackable);
    if (it.Stackable) { ImGui::SameLine(); ImGui::SetNextItemWidth(120.0f); ImGui::DragInt("max per stack", &it.MaxStack, 1.0f, 1, 100000); }
    ImGui::Checkbox("Consumable", &it.Consumable);
    ImGui::SetItemTooltip("Used up when used (npc:use): potions, food, scrolls");

    ImGui::SeparatorText("Stats");
    ImGui::TextDisabled("Its own numbers, for scripts: damage_min, armour, range... (db.item(id).stats)");
    std::string removeStat, renameFrom, renameTo;
    for (auto& [key, value] : it.Stats) {
        ImGui::PushID(key.c_str());
        std::string k = key;
        ImGui::SetNextItemWidth(160.0f);
        if (Text("##key", k) && !k.empty() && !it.Stats.count(k)) { renameFrom = key; renameTo = k; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("##value", &value, 0.1f);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeStat = key;
        ImGui::PopID();
    }
    if (!renameFrom.empty()) { const float v = it.Stats[renameFrom]; it.Stats.erase(renameFrom); it.Stats[renameTo] = v; }
    if (!removeStat.empty()) it.Stats.erase(removeStat);
    if (ImGui::SmallButton("+ Stat")) {
        std::string k = "stat";
        for (int n = 2; it.Stats.count(k); n++) k = "stat_" + std::to_string(n);
        it.Stats[k] = 0.0f;
    }

    ImGui::SeparatorText("Effects");
    int removeFx = -1;
    for (int i = 0; i < (int)it.Effects.size(); i++) {
        ItemEffect& e = it.Effects[i];
        ImGui::PushID(i);
        const char* triggers[] = { "On use", "Equipped", "On hit" };
        const char* kinds[] = { "Add", "Modifier", "Over time", "Event" };
        int t = (int)e.Trigger, k = (int)e.Kind;
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::Combo("##trigger", &t, triggers, 3)) e.Trigger = (EffectTrigger)t;
        ImGui::SetItemTooltip("On use: npc:use(item).  Equipped: while equipped (Add / Event: once, when equipped).\n"
                              "On hit: npc:apply(item, \"hit\") on what a weapon hits.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::Combo("##kind", &k, kinds, 4)) e.Kind = (EffectKind)k;
        ImGui::SetItemTooltip("Add: at once.  Modifier: + amount while it lasts.\nOver time: amount per second.  Event: a script event.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170.0f);
        if (e.Kind == EffectKind::Event) Text("##event", e.Event);
        else StatCombo("##stat", e.Stat);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat("##amount", &e.Amount, 0.1f, 0.0f, 0.0f, "%+g");
        if ((e.Kind == EffectKind::Modifier && e.Trigger != EffectTrigger::Equip) || e.Kind == EffectKind::OverTime) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("##duration", &e.Duration, 0.1f, 0.0f, 3600.0f, e.Duration > 0.0f ? "%g s" : "for good");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeFx = i;
        ImGui::TextDisabled("    %s", EffectSummary(e).c_str());
        ImGui::PopID();
    }
    if (removeFx >= 0) it.Effects.erase(it.Effects.begin() + removeFx);
    if (ImGui::SmallButton("+ Effect")) {
        ItemEffect e;
        e.Stat = m_Db.DeathStat;
        if (it.Slot.empty() == false) e.Trigger = EffectTrigger::Equip, e.Kind = EffectKind::Modifier;
        it.Effects.push_back(e);
    }

    // who carries it
    std::string carriers;
    for (const auto& n : m_Db.Npcs)
        for (const auto& e : n.Inventory)
            if (e.Item == it.Id) { carriers += (carriers.empty() ? "" : ", ") + n.Name; break; }
    if (!carriers.empty()) { ImGui::Spacing(); ImGui::TextDisabled("Carried by: %s", carriers.c_str()); }
    ImGui::PopID();
}

void DatabaseEditor::DrawNpc(NpcDef& n) {
    ImGui::PushID(&n);
    IdField("npc", n.Id);
    Text("Name", n.Name);
    ImGui::Text("Description");
    Text("##description", n.Description, true);

    ImGui::SeparatorText("Looks and behaviour");
    AssetCombo("Model", n.Model, { "Models", "Prefabs" }, kModels);
    ImGui::SetItemTooltip("A model (scaled to the height) or a prefab (placed as it is). None: the entity's own looks.");
    ImGui::DragFloat("Height", &n.Height, 0.01f, 0.1f, 50.0f, "%.2f m");
    AssetCombo("Animation graph", n.AnimGraph, { "AnimGraphs" }, KindBit(AssetBrowser::Kind::AnimGraph));
    if (!n.AnimGraph.empty() && OnOpenAnimGraph) { ImGui::SameLine(); if (ImGui::SmallButton("Edit##graph")) OnOpenAnimGraph(n.AnimGraph); }
    AssetCombo("Script", n.Script, { "Scripts" }, KindBit(AssetBrowser::Kind::Script));
    ImGui::SetItemTooltip("Its behaviour (self.npc is its state)");
    AssetCombo("Dialogue", n.Dialogue, { "Dialogues" }, KindBit(AssetBrowser::Kind::Dialogue));
    ImGui::SetItemTooltip("What it says (npc.lua plays it; scripts read self.npc.data.dialogue)");
    if (!n.Dialogue.empty() && OnOpenDialogue) { ImGui::SameLine(); if (ImGui::SmallButton("Edit##dialogue")) OnOpenDialogue(n.Dialogue); }
    ImGui::Checkbox("Capsule body", &n.Collider);
    ImGui::SetItemTooltip("A kinematic capsule as tall as the NPC: clicks, raycasts and triggers find it");
    if (n.Collider) { ImGui::SameLine(); ImGui::SetNextItemWidth(120.0f); ImGui::DragFloat("radius", &n.Radius, 0.01f, 0.05f, 10.0f, "%.2f m"); }
    ImGui::Checkbox("Removed when it dies", &n.RemoveOnDeath);
    ImGui::SetItemTooltip("Off: the body stays (a script can play a death animation)");
    Text("Death event", n.DeathEvent);
    ImGui::SetItemTooltip("Sent to every script when it dies, as well as \"npc_died\" (e.g. \"boss_defeated\")");

    ImGui::SeparatorText("Stats");
    for (const auto& s : m_Db.Stats) {
        ImGui::PushID(s.Id.c_str());
        auto found = n.Stats.find(s.Id);
        bool own = found != n.Stats.end();
        if (ImGui::Checkbox("##own", &own)) {
            if (own) n.Stats[s.Id] = s.Default;
            else n.Stats.erase(s.Id);
            found = n.Stats.find(s.Id);
        }
        ImGui::SetItemTooltip(own ? "Its own value (untick: the stat's default)" : "Tick to give it its own value");
        ImGui::SameLine();
        if (found != n.Stats.end()) ImGui::DragFloat(s.Name.c_str(), &found->second, 0.1f, s.Min, s.Max);
        else {
            ImGui::BeginDisabled();
            float d = s.Default;
            ImGui::DragFloat(s.Name.c_str(), &d);
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    if (m_Db.Stats.empty()) ImGui::TextDisabled("No stats yet: add them in the Stats tab");

    ImGui::SeparatorText("Inventory");
    ImGui::TextDisabled("What it carries when it comes to life (chance rolled once). Drop: falls to the ground when it dies.");
    int removeInv = -1;
    for (int i = 0; i < (int)n.Inventory.size(); i++) {
        InventoryEntry& e = n.Inventory[i];
        ImGui::PushID(i);
        const ItemDef* item = m_Db.Item(e.Item);
        Icon(item ? item->Icon : "", ImGui::GetFrameHeight());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(190.0f);
        ItemCombo("##item", e.Item);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        if (ImGui::DragInt("##count", &e.Count, 0.2f, 1, 100000, "x%d")) e.Count = std::max(1, e.Count);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        float pct = e.Chance * 100.0f;
        if (ImGui::SliderFloat("##chance", &pct, 0.0f, 100.0f, "%.0f%%")) e.Chance = pct / 100.0f;
        ImGui::SetItemTooltip("The chance it has it");
        ImGui::SameLine();
        ImGui::Checkbox("drop", &e.Drop);
        ImGui::SameLine();
        ImGui::BeginDisabled(!item || item->Slot.empty());
        ImGui::Checkbox("equipped", &e.Equipped);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeInv = i;
        ImGui::PopID();
    }
    if (removeInv >= 0) n.Inventory.erase(n.Inventory.begin() + removeInv);
    if (ImGui::SmallButton("+ Item")) n.Inventory.push_back({ m_Db.Items.empty() ? std::string() : m_Db.Items.front().Id });
    ImGui::SetItemTooltip("Or drag an item from the Items tab onto an inventory line's item");

    ImGui::Spacing();
    ImGui::Separator();
    if (OnPlaceNpc && ImGui::Button("Place in scene")) OnPlaceNpc(n.Id);
    ImGui::SetItemTooltip("A new entity for it in front of the camera (or drag it from the list into the Viewport)");
    ImGui::PopID();
}

void DatabaseEditor::DrawStat(StatDef& s) {
    ImGui::PushID(&s);
    IdField("stat", s.Id);
    Text("Name", s.Name);
    ImGui::DragFloat("Default", &s.Default, 0.1f);
    ImGui::DragFloat("Minimum", &s.Min, 0.1f);
    ImGui::DragFloat("Maximum", &s.Max, 0.1f);
    std::string maxStat = s.MaxStat;
    if (StatCombo("Ceiling", maxStat, "(none: the maximum)") && maxStat != s.Id) s.MaxStat = maxStat;
    ImGui::SetItemTooltip("Another stat whose value is this one's maximum (health up to max_health)");
    ImGui::Text("Description");
    Text("##description", s.Description, true);
    if (m_Db.DeathStat == s.Id) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "The death stat: an NPC dies when it reaches %g.", s.Min);
    ImGui::PopID();
}

void DatabaseEditor::DrawProblems() {
    if (m_ProblemsFrame-- <= 0) { m_Problems = m_Db.Issues(m_AssetDir); m_ProblemsFrame = 30; }   // file checks: twice a second
    if (m_Problems.empty()) ImGui::TextDisabled("No problems.  %zu items, %zu NPCs, %zu stats", m_Db.Items.size(), m_Db.Npcs.size(), m_Db.Stats.size());
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%zu problem%s (hover)", m_Problems.size(), m_Problems.size() == 1 ? "" : "s");
        if (ImGui::BeginItemTooltip()) {
            for (const auto& p : m_Problems) ImGui::BulletText("%s", p.c_str());
            ImGui::EndTooltip();
        }
    }
    if (!m_Status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("   %s", m_Status.c_str()); }
}
