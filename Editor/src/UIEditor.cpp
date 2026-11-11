#include "UIEditor.h"
#include "AssetBrowser.h"

#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace Engine;

namespace {
    const char* kAddTypes[] = { "label", "button", "image", "bar", "slot", "row", "column", "scroll", "space", "separator" };

    struct Aspect { const char* Name; float Value; };
    const Aspect kAspects[] = { { "16:9", 16.0f / 9.0f }, { "16:10", 1.6f }, { "4:3", 4.0f / 3.0f }, { "21:9", 21.0f / 9.0f }, { "9:16 (phone)", 9.0f / 16.0f } };

    const char* TypeLabel(const std::string& t) {
        if (t == "panel") return "Panel";    if (t == "row") return "Row";       if (t == "column") return "Column";
        if (t == "scroll") return "Scroll";  if (t == "label") return "Label";   if (t == "button") return "Button";
        if (t == "image") return "Image";    if (t == "bar") return "Bar";       if (t == "slot") return "Slot";
        if (t == "space") return "Space";    if (t == "separator") return "Separator";
        return "?";
    }
    ImVec4 TypeColor(const std::string& t) {
        if (t == "panel") return ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
        if (t == "row" || t == "column" || t == "scroll") return ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
        if (t == "button") return ImVec4(0.6f, 0.95f, 0.6f, 1.0f);
        if (t == "space" || t == "separator") return ImVec4(0.6f, 0.6f, 0.65f, 1.0f);
        return ImVec4(0.92f, 0.92f, 0.95f, 1.0f);
    }

    int ResizeCallback(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* s = static_cast<std::string*>(data->UserData);
            s->resize(data->BufTextLen);
            data->Buf = s->data();
        }
        return 0;
    }
    bool InputStr(const char* label, std::string& s, bool multiline = false, float height = 0.0f, const char* hint = nullptr) {
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
        if (multiline) return ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, ImVec2(-1, height), flags, ResizeCallback, &s);
        if (hint) return ImGui::InputTextWithHint(label, hint, s.data(), s.capacity() + 1, flags, ResizeCallback, &s);
        return ImGui::InputText(label, s.data(), s.capacity() + 1, flags, ResizeCallback, &s);
    }

    void Help(const char* text) {
        if (!text) return;
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // What a node shows, for the tree: its text, title or image, first line, shortened
    std::string Summary(const UINode& n) {
        std::string s;
        for (const char* k : { "text", "title", "path", "icon", "value" }) {
            auto it = n.Props.find(k);
            if (it != n.Props.end() && it->is_string() && !it->get<std::string>().empty()) { s = it->get<std::string>(); break; }
            if (it != n.Props.end() && it->is_number()) { s = it->dump(); break; }
        }
        if (const size_t nl = s.find('\n'); nl != std::string::npos) s = s.substr(0, nl) + " ...";
        if (s.size() > 34) s = s.substr(0, 32) + "...";
        if (auto r = n.Props.find("repeat"); r != n.Props.end() && !r->is_null()) s = "x[" + (r->is_string() ? r->get<std::string>() : r->dump()) + "] " + s;
        if (n.Props.contains("visible")) s = "? " + s;
        return s;
    }

    ImVec2 Lerp(ImVec2 a, ImVec2 b, float tx, float ty) { return ImVec2(a.x + (b.x - a.x) * tx, a.y + (b.y - a.y) * ty); }
}

UIEditor::UIEditor(std::string assetDir) : m_AssetDir(std::move(assetDir)) {
    New();
}

// ------------------------------------------------------------------ file
bool UIEditor::IsDirty() const { return Text() != m_SavedText; }

std::vector<std::string> UIEditor::ListFiles() const {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path root = fs::u8path(m_AssetDir + "UI");
    for (const auto& f : fs::recursive_directory_iterator(root, ec))
        if (f.is_regular_file() && f.path().extension() == ".json")
            out.push_back("UI/" + fs::relative(f.path(), root, ec).generic_string());
    std::sort(out.begin(), out.end());
    return out;
}

void UIEditor::New() {
    if (IsDirty() && !m_SavedText.empty()) { m_PendingNew = true; m_PendingOpen.clear(); m_AskUnsaved = true; return; }
    m_Doc = UIDocument{};
    UINode panel;
    panel.Type = "panel";
    panel.Name = "panel";
    panel.Props = { { "title", "New panel" }, { "w", 360 } };
    UINode label;
    label.Type = "label";
    label.Props = { { "text", "Some text. {#ffcc66}Markup{/} and {b}{=\"expressions\"}{/b} work here." } };
    panel.Children.push_back(label);
    m_Doc.Widgets.push_back(panel);
    m_Doc.AssignIds();
    m_Path.clear();
    m_SavedText = Text();
    m_Selected = m_Doc.Widgets[0].Id;
    m_Status.clear();
    ResetHistory();
}

void UIEditor::Open(const std::string& path) {
    if (path == m_Path && !IsDirty()) return;
    if (IsDirty()) { m_PendingOpen = path; m_PendingNew = false; m_AskUnsaved = true; return; }
    Load(path);
}

bool UIEditor::Load(const std::string& path) {
    std::string error;
    auto doc = UIDocument::Load(m_AssetDir + path, &error);
    if (!doc) { m_Status = "Can't open " + path + ": " + error; return false; }
    m_Doc = *doc;
    m_Path = path;
    m_SavedText = Text();
    m_Selected = m_Doc.Widgets.empty() ? 0 : m_Doc.Widgets[0].Id;
    m_Status = "Opened " + path;
    m_Preview.Reset();
    ResetHistory();
    Publish();
    return true;
}

bool UIEditor::SaveTo(const std::string& path) {
    if (!m_Doc.Save(m_AssetDir + path)) { m_Status = "Couldn't write " + path; return false; }
    m_Path = path;
    m_SavedText = Text();
    m_Status = "Saved " + path;
    Publish();
    return true;
}

bool UIEditor::SaveIfNamed() { return !m_Path.empty() && SaveTo(m_Path); }

void UIEditor::SelectName(const std::string& name) {
    std::function<bool(std::vector<UINode>&)> find = [&](std::vector<UINode>& list) {
        for (UINode& n : list) {
            if (n.Name == name) { m_Selected = m_PendingSelect = n.Id; m_FrameSelected = true; return true; }
            if (find(n.Children)) return true;
        }
        return false;
    };
    find(m_Doc.Widgets);
}

// ------------------------------------------------------------------ undo, live copy
void UIEditor::ResetHistory() {
    m_History.assign(1, { Text(), m_Doc });
    m_HistoryPos = 0;
}

void UIEditor::Commit() {
    std::string s = Text();
    if (m_HistoryPos >= 0 && m_History[m_HistoryPos].first == s) return;
    m_History.resize(m_HistoryPos + 1);
    m_History.emplace_back(std::move(s), m_Doc);
    if (m_History.size() > 200) m_History.erase(m_History.begin());
    m_HistoryPos = (int)m_History.size() - 1;
}

void UIEditor::Undo() {
    Commit();
    if (m_HistoryPos <= 0) return;
    m_Doc = m_History[--m_HistoryPos].second;
    if (!m_Doc.Find(m_Selected)) m_Selected = 0;
}

void UIEditor::Redo() {
    if (m_HistoryPos + 1 >= (int)m_History.size()) return;
    m_Doc = m_History[++m_HistoryPos].second;
    if (!m_Doc.Find(m_Selected)) m_Selected = 0;
}

void UIEditor::Publish() {
    if (m_Path.empty() || !OnChanged) return;
    std::string key = m_Path + "|" + Text();
    if (key == m_PublishedText) return;
    m_PublishedText = std::move(key);
    OnChanged(m_Path, std::make_shared<const UIDocument>(m_Doc));
}

// ------------------------------------------------------------------ tree
bool UIEditor::Locate(uint32_t id, std::vector<UINode>*& list, size_t& index) {
    std::function<bool(std::vector<UINode>&)> walk = [&](std::vector<UINode>& l) {
        for (size_t i = 0; i < l.size(); i++) {
            if (l[i].Id == id) { list = &l; index = i; return true; }
            if (walk(l[i].Children)) return true;
        }
        return false;
    };
    return walk(m_Doc.Widgets);
}

UINode* UIEditor::ParentOf(uint32_t id) {
    UINode* found = nullptr;
    std::function<void(UINode&)> walk = [&](UINode& n) {
        for (UINode& c : n.Children) { if (c.Id == id) found = &n; walk(c); }
    };
    for (UINode& n : m_Doc.Widgets) walk(n);
    return found;
}

bool UIEditor::Contains(const UINode& root, uint32_t id) const {
    if (root.Id == id) return true;
    for (const UINode& c : root.Children) if (Contains(c, id)) return true;
    return false;
}

uint32_t UIEditor::TopPanelOf(uint32_t id) {
    for (const UINode& n : m_Doc.Widgets) if (Contains(n, id)) return n.Id;
    return 0;
}

std::string UIEditor::UniqueName(const std::string& base) const {
    std::vector<std::string> used;
    std::function<void(const std::vector<UINode>&)> walk = [&](const std::vector<UINode>& l) {
        for (const UINode& n : l) { used.push_back(n.Name); walk(n.Children); }
    };
    walk(m_Doc.Widgets);
    for (int i = 1;; i++) {
        std::string name = base + (i == 1 ? std::string() : std::to_string(i));
        if (std::find(used.begin(), used.end(), name) == used.end()) return name;
    }
}

UINode UIEditor::MakeNode(const std::string& type) {
    UINode n;
    n.Type = type;
    if (type == "panel") { n.Name = UniqueName("panel"); n.Props = { { "title", "Panel" }, { "w", 320 } }; }
    else if (type == "label") n.Props = { { "text", "Text" } };
    else if (type == "button") { n.Name = UniqueName("button"); n.Props = { { "text", "Button" } }; }
    else if (type == "image") n.Props = { { "path", "" }, { "w", 64 } };
    else if (type == "bar") n.Props = { { "value", 0.6 }, { "text", "" } };
    else if (type == "slot") n.Name = UniqueName("slot");
    else if (type == "scroll") n.Props = { { "height", 200 } };
    else if (type == "space") n.Props = { { "h", 8 } };
    return n;
}

void UIEditor::Renumber(UINode& n) {
    n.Id = m_Doc.NextId();
    for (UINode& c : n.Children) Renumber(c);
}

// inside: into the selected container (else after the selection, in its list). Panels always go at the top.
void UIEditor::Add(const std::string& type, bool inside) {
    UINode n = MakeNode(type);
    Renumber(n);
    const uint32_t id = n.Id;
    if (type == "panel") {
        const uint32_t top = TopPanelOf(m_Selected);
        auto at = std::find_if(m_Doc.Widgets.begin(), m_Doc.Widgets.end(), [&](const UINode& p) { return p.Id == top; });
        m_Doc.Widgets.insert(at == m_Doc.Widgets.end() ? at : at + 1, std::move(n));
    }
    else {
        UINode* sel = m_Doc.Find(m_Selected);
        if (!sel) {
            if (m_Doc.Widgets.empty()) { UINode p = MakeNode("panel"); Renumber(p); m_Doc.Widgets.push_back(p); }
            sel = &m_Doc.Widgets.back();
            inside = true;
        }
        if (sel->Type == "panel") inside = true;    // nothing goes beside a panel but panels
        if (inside && sel->IsContainer()) sel->Children.push_back(std::move(n));
        else {
            std::vector<UINode>* list = nullptr;
            size_t index = 0;
            if (Locate(m_Selected, list, index)) list->insert(list->begin() + index + 1, std::move(n));
        }
    }
    m_Selected = m_PendingSelect = id;
    Commit();
}

void UIEditor::Delete(uint32_t id) {
    std::vector<UINode>* list = nullptr;
    size_t index = 0;
    if (!Locate(id, list, index)) return;
    UINode* parent = ParentOf(id);
    const uint32_t next = index + 1 < list->size() ? (*list)[index + 1].Id : index > 0 ? (*list)[index - 1].Id : parent ? parent->Id : 0;
    list->erase(list->begin() + index);
    m_Selected = next;
    Commit();
}

void UIEditor::Duplicate(uint32_t id) {
    std::vector<UINode>* list = nullptr;
    size_t index = 0;
    if (!Locate(id, list, index)) return;
    UINode copy = (*list)[index];
    Renumber(copy);
    if (!copy.Name.empty()) copy.Name = UniqueName(copy.Name);
    if (copy.Type == "panel") {
        for (const char* k : { "x", "y" })
            if (auto it = copy.Props.find(k); it != copy.Props.end() && it->is_number()) *it = it->get<float>() + 0.03f;
            else if (it == copy.Props.end()) copy.Props[k] = 0.53f;
    }
    const uint32_t newId = copy.Id;
    list->insert(list->begin() + index + 1, std::move(copy));
    m_Selected = newId;
    Commit();
}

void UIEditor::Move(uint32_t id, int dir) {
    std::vector<UINode>* list = nullptr;
    size_t index = 0;
    if (!Locate(id, list, index)) return;
    const long to = (long)index + dir;
    if (to < 0 || to >= (long)list->size()) return;
    std::swap((*list)[index], (*list)[to]);
    Commit();
}

// A tree drop: into `target` if it is a container, else before it. Panels stay at the top.
void UIEditor::MoveTo(uint32_t id, uint32_t target) {
    if (id == target) return;
    UINode* node = m_Doc.Find(id);
    UINode* dest = m_Doc.Find(target);
    if (!node || !dest || Contains(*node, target)) return;   // not into itself
    const bool panel = node->Type == "panel";
    if (panel) target = TopPanelOf(target);
    else if (dest->Type != "panel" && !dest->IsContainer() && !ParentOf(target)) return;
    UINode moving = *node;
    std::vector<UINode>* list = nullptr;
    size_t index = 0;
    Locate(id, list, index);
    list->erase(list->begin() + index);
    dest = m_Doc.Find(target);                              // pointers moved with the erase
    if (!dest) return;
    if (!panel && dest->IsContainer()) dest->Children.push_back(std::move(moving));
    else if (Locate(target, list, index)) list->insert(list->begin() + index, std::move(moving));
    m_Selected = m_PendingSelect = id;
    Commit();
}

// ------------------------------------------------------------------ the window
void UIEditor::Draw(bool* open) {
    const std::string name = m_Path.empty() ? std::string("(unsaved)") : fs::path(m_Path).filename().string();
    const std::string title = "UI Editor - " + name + (IsDirty() ? " *" : "") + "###UIEditor";
    ImGui::SetNextWindowSize(ImVec2(1300, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), open, ImGuiWindowFlags_MenuBar)) {
        m_Focused = false;
        ImGui::End();
        return;
    }
    m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    const ImGuiIO& io = ImGui::GetIO();
    if (m_Focused) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) { if (m_Path.empty()) m_AskSaveAs = true; else SaveTo(m_Path); }
        if (!io.WantTextInput) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) Redo(); else Undo(); }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && m_Selected) Duplicate(m_Selected);
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_Selected) Delete(m_Selected);
        }
    }

    DrawMenuBar();

    const float fullW = ImGui::GetContentRegionAvail().x;
    const float treeW = std::clamp(fullW * 0.22f, 200.0f, 340.0f), propsW = std::clamp(fullW * 0.27f, 280.0f, 420.0f);
    ImGui::BeginChild("##tree", ImVec2(treeW, 0), ImGuiChildFlags_Border);
    DrawTree();
    ImGui::EndChild();
    for (auto& f : m_Later) f();
    m_Later.clear();
    ImGui::SameLine();
    ImGui::BeginChild("##preview", ImVec2(ImGui::GetContentRegionAvail().x - propsW - 6.0f, 0), ImGuiChildFlags_Border,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawPreview();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##props", ImVec2(0, 0), ImGuiChildFlags_Border);
    DrawProperties();
    ImGui::EndChild();

    DrawPopups();
    if (m_WantCommit && !ImGui::IsAnyItemActive()) { Commit(); m_WantCommit = false; }
    Publish();                                 // live: a running game shows every change (also mid-drag)
    ImGui::End();
}

void UIEditor::DrawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New")) New();
        if (ImGui::BeginMenu("Open")) {
            const auto files = ListFiles();
            if (files.empty()) ImGui::TextDisabled("No layouts in Assets/UI yet");
            for (const std::string& f : files)
                if (ImGui::MenuItem(f.substr(3).c_str(), nullptr, f == m_Path)) Open(f);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) { if (m_Path.empty()) m_AskSaveAs = true; else SaveTo(m_Path); }
        if (ImGui::MenuItem("Save As...")) m_AskSaveAs = true;
        if (ImGui::MenuItem("Revert", nullptr, false, !m_Path.empty() && IsDirty())) { m_SavedText = Text(); Load(m_Path); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_HistoryPos > 0)) Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_HistoryPos + 1 < (int)m_History.size())) Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, m_Selected != 0)) Duplicate(m_Selected);
        if (ImGui::MenuItem("Delete", "Delete", false, m_Selected != 0)) Delete(m_Selected);
        if (ImGui::MenuItem("Move Up", nullptr, false, m_Selected != 0)) Move(m_Selected, -1);
        if (ImGui::MenuItem("Move Down", nullptr, false, m_Selected != 0)) Move(m_Selected, 1);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        DrawAddMenu(true);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (!m_Path.empty()) ImGui::TextDisabled("ui.show(\"%s\")", m_Path.c_str());
    else ImGui::TextDisabled("not saved yet");
    if (!m_Status.empty()) { ImGui::Separator(); ImGui::TextDisabled("%s", m_Status.c_str()); }
    ImGui::EndMenuBar();
}

void UIEditor::DrawAddMenu(bool inside) {
    if (ImGui::MenuItem("Panel")) m_Later.push_back([this]() { Add("panel", false); });
    ImGui::Separator();
    for (const char* t : kAddTypes) {
        const std::string type = t;
        if (ImGui::MenuItem(TypeLabel(type))) m_Later.push_back([this, type, inside]() { Add(type, inside); });
    }
}

void UIEditor::DrawTree() {
    if (ImGui::Button("+ Add")) ImGui::OpenPopup("##add");
    if (ImGui::BeginPopup("##add")) {
        const UINode* sel = m_Doc.Find(m_Selected);
        ImGui::TextDisabled(sel && sel->IsContainer() ? "Into the selected %s" : sel ? "After the selected %s" : "New", sel ? TypeLabel(sel->Type) : "");
        DrawAddMenu(true);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_Selected);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) m_Later.push_back([this]() { Move(m_Selected, -1); });
    ImGui::SameLine();
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) m_Later.push_back([this]() { Move(m_Selected, 1); });
    ImGui::EndDisabled();
    ImGui::Separator();

    // problems (cached per document text)
    const std::string text = Text();
    if (text != m_IssuesFor) { m_IssuesFor = text; m_Issues = m_Doc.Issues(m_AssetDir); }
    const float problemsH = m_Issues.empty() ? ImGui::GetFrameHeight() : std::min(160.0f, 26.0f + 20.0f * (float)m_Issues.size());

    ImGui::BeginChild("##nodes", ImVec2(0, -problemsH));
    for (UINode& n : m_Doc.Widgets) DrawTreeNode(n);
    // the empty space below: click to select nothing
    ImGui::Dummy(ImVec2(-1, std::max(20.0f, ImGui::GetContentRegionAvail().y)));
    if (ImGui::IsItemClicked()) m_Selected = 0;
    if (ImGui::BeginPopupContextItem("##treebg")) {
        ImGui::TextDisabled("Add");
        DrawAddMenu(false);
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    m_PendingSelect = 0;

    if (m_Issues.empty()) ImGui::TextDisabled("No problems");
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%zu problem%s", m_Issues.size(), m_Issues.size() == 1 ? "" : "s");
        ImGui::BeginChild("##problems");
        for (const std::string& p : m_Issues) ImGui::TextWrapped("- %s", p.c_str());
        ImGui::EndChild();
    }
}

void UIEditor::DrawTreeNode(UINode& n) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_FramePadding;
    if (n.Id == m_Selected) flags |= ImGuiTreeNodeFlags_Selected;
    const bool container = n.IsContainer();
    if (container) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    else flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (m_PendingSelect && n.Id != m_PendingSelect && Contains(n, m_PendingSelect)) ImGui::SetNextItemOpen(true);

    ImGui::PushID((int)n.Id);
    ImGui::PushStyleColor(ImGuiCol_Text, TypeColor(n.Type));
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", TypeLabel(n.Type));
    ImGui::PopStyleColor();
    if (n.Id == m_PendingSelect) ImGui::SetScrollHereY(0.5f);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) m_Selected = n.Id;
    if (ImGui::IsItemHovered()) m_Hovered = n.Id;
    // what it is, after the type
    ImGui::SameLine();
    if (!n.Name.empty()) { ImGui::TextUnformatted(n.Name.c_str()); ImGui::SameLine(); }
    ImGui::TextDisabled("%s", Summary(n).c_str());

    if (ImGui::BeginDragDropSource()) {
        const uint32_t id = n.Id;
        ImGui::SetDragDropPayload(NodePayload, &id, sizeof id);
        ImGui::Text("%s %s", TypeLabel(n.Type), n.Name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(NodePayload)) {
            uint32_t from = 0;
            std::memcpy(&from, p->Data, sizeof from);
            const uint32_t to = n.Id;
            m_Later.push_back([this, from, to]() { MoveTo(from, to); });
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##ctx")) {
        m_Selected = n.Id;
        const uint32_t id = n.Id;
        if (container && ImGui::BeginMenu("Add inside")) {
            for (const char* t : kAddTypes) {
                const std::string type = t;
                if (ImGui::MenuItem(TypeLabel(type))) m_Later.push_back([this, type]() { Add(type, true); });
            }
            ImGui::EndMenu();
        }
        if (n.Type != "panel" && ImGui::BeginMenu("Add after")) {
            for (const char* t : kAddTypes) {
                const std::string type = t;
                if (ImGui::MenuItem(TypeLabel(type))) m_Later.push_back([this, type]() { Add(type, false); });
            }
            ImGui::EndMenu();
        }
        if (n.Type == "panel" && ImGui::MenuItem("Add panel")) m_Later.push_back([this]() { Add("panel", false); });
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) m_Later.push_back([this, id]() { Duplicate(id); });
        if (ImGui::MenuItem("Move Up")) m_Later.push_back([this, id]() { Move(id, -1); });
        if (ImGui::MenuItem("Move Down")) m_Later.push_back([this, id]() { Move(id, 1); });
        if (ImGui::MenuItem("Delete", "Delete")) m_Later.push_back([this, id]() { Delete(id); });
        ImGui::EndPopup();
    }
    if (container && open) {
        for (UINode& c : n.Children) DrawTreeNode(c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// ------------------------------------------------------------------ preview
void UIEditor::DrawPreview() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextItemWidth(100);
    if (ImGui::BeginCombo("##aspect", kAspects[m_Aspect].Name)) {
        for (int i = 0; i < (int)std::size(kAspects); i++)
            if (ImGui::Selectable(kAspects[i].Name, i == m_Aspect)) m_Aspect = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Try it", &m_Try);
    ImGui::SetItemTooltip("The pointer works the layout as in the game: buttons light up and click, and their on_click runs on the\n"
                          "preview values. Off: click a widget to select it, drag to move its panel.");
    ImGui::SameLine();
    if (ImGui::Button("Reset")) m_Preview.Reset();
    ImGui::SetItemTooltip("The preview values as written in the Preview data tab (Try it may have changed them)");
    ImGui::SameLine();
    ImGui::Checkbox("Outlines", &m_Outlines);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) { m_Zoom = 1.0f; m_Pan = ImVec2(0, 0); }
    ImGui::SetItemTooltip("Show the whole view (Home)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_Selected);
    if (ImGui::Button("Frame")) m_FrameSelected = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Zoom onto the selected widget (F)");
    ImGui::SameLine();
    ImGui::TextDisabled("%.0f%%", m_Zoom * 100.0f);
    ImGui::SetItemTooltip("Wheel zooms around the pointer (Ctrl+wheel while trying it),\nright or middle drag pans");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 1.2f;
    avail.y -= footer;
    if (avail.x < 60 || avail.y < 60) return;
    const float aspect = kAspects[m_Aspect].Value;
    ImVec2 fit(avail.x, avail.x / aspect);              // the whole view in the area (zoom 1)
    if (fit.y > avail.y) fit = ImVec2(avail.y * aspect, avail.y);
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 centre(cursor.x + avail.x * 0.5f, cursor.y + avail.y * 0.5f);

    ImGui::InvisibleButton("##view", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();

    // ---- view: wheel zooms around the pointer (Ctrl+wheel in Try it, where the wheel scrolls the UI), right / middle drag
    // pans, F frames the selection, Home fits
    if (m_Focused && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.KeyCtrl && m_Selected) m_FrameSelected = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) { m_Zoom = 1.0f; m_Pan = ImVec2(0, 0); }
    }
    const float wheel = hovered && (!m_Try || io.KeyCtrl) ? io.MouseWheel : 0.0f;
    if (wheel != 0.0f) {
        const float old = m_Zoom;
        m_Zoom = std::clamp(m_Zoom * std::pow(1.15f, wheel), 0.25f, 12.0f);
        // keep the point under the pointer where it is: pan is the view centre's offset from the area centre
        const ImVec2 rel(io.MousePos.x - centre.x - m_Pan.x, io.MousePos.y - centre.y - m_Pan.y);
        m_Pan.x += rel.x - rel.x * m_Zoom / old;
        m_Pan.y += rel.y - rel.y * m_Zoom / old;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))) {
        m_Pan.x += io.MouseDelta.x;
        m_Pan.y += io.MouseDelta.y;
    }
    if (m_FrameSelected && !m_Rects.empty()) {        // (last frame's rectangles: none yet on the first frame, so it waits)
        m_FrameSelected = false;
        bool any = false;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;   // every copy of it (repeats)
        for (const UINodeRect& r : m_Rects) {
            if (r.Id != m_Selected || r.W <= 0 || r.H <= 0) continue;
            x0 = std::min(x0, r.X); y0 = std::min(y0, r.Y); x1 = std::max(x1, r.X + r.W); y1 = std::max(y1, r.Y + r.H);
            any = true;
        }
        if (any) {
            m_Zoom = std::clamp(std::min(avail.x * 0.7f / ((x1 - x0) * fit.x), avail.y * 0.7f / ((y1 - y0) * fit.y)), 0.25f, 12.0f);
            // its middle at the area's centre
            m_Pan = ImVec2((0.5f - (x0 + x1) * 0.5f) * fit.x * m_Zoom, (0.5f - (y0 + y1) * 0.5f) * fit.y * m_Zoom);
        }
    }
    const ImVec2 size(fit.x * m_Zoom, fit.y * m_Zoom);
    const ImVec2 vmin(centre.x + m_Pan.x - size.x * 0.5f, centre.y + m_Pan.y - size.y * 0.5f);
    const ImVec2 vmax(vmin.x + size.x, vmin.y + size.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(cursor, ImVec2(cursor.x + avail.x, cursor.y + avail.y), true);   // zoomed in, it spills past the area
    // a stand-in for the game behind the UI
    dl->AddRectFilledMultiColor(vmin, vmax, IM_COL32(64, 84, 104, 255), IM_COL32(64, 84, 104, 255), IM_COL32(30, 36, 44, 255), IM_COL32(30, 36, 44, 255));
    dl->AddLine(ImVec2((vmin.x + vmax.x) * 0.5f, vmin.y), ImVec2((vmin.x + vmax.x) * 0.5f, vmax.y), IM_COL32(255, 255, 255, 18));
    dl->AddLine(ImVec2(vmin.x, (vmin.y + vmax.y) * 0.5f), ImVec2(vmax.x, (vmin.y + vmax.y) * 0.5f), IM_COL32(255, 255, 255, 18));

    const ImVec2 mouse = io.MousePos;
    const float mx = (mouse.x - vmin.x) / size.x, my = (mouse.y - vmin.y) / size.y;
    const bool inView = hovered && mx >= 0 && mx <= 1 && my >= 0 && my <= 1;

    UIPointerState p;
    if (m_Try) {
        p.X = mx; p.Y = my; p.Inside = inView;
        p.Down = inView && io.MouseDown[0];
        p.Wheel = inView && !io.KeyCtrl ? io.MouseWheel : 0.0f;   // Ctrl+wheel zooms the preview instead
    }
    m_Items.clear();
    m_Rects.clear();
    m_Preview.RunActions = m_Try;
    m_Preview.SetVars(m_Doc.Preview);
    m_Preview.BeginFrame();
    m_UI.BeginFrame(m_Items, aspect, p, io.DeltaTime);
    m_PreviewError.clear();
    UIDocResult result;
    try { result = DrawUIDocument(m_UI, m_Doc, m_Preview, "preview", m_Items, &m_Rects); }
    catch (const std::exception& e) { m_PreviewError = e.what(); }
    m_UI.EndFrame();
    FinishUIRects(m_Items, m_Rects);
    if (m_Try && !result.Clicked.empty())
        m_Status = "Clicked: " + result.Clicked + (result.Index ? " (entry " + std::to_string(result.Index) + ")" : std::string());

    dl->PushClipRect(vmin, vmax, true);
    DrawScriptUI(dl, m_Fonts, m_Items, vmin, size, m_Textures);

    // ---- select and move (when not trying it)
    auto rectAt = [&](float x, float y) -> uint32_t {
        const UINodeRect* best = nullptr;
        for (const UINodeRect& r : m_Rects) {
            if (x < r.X || x > r.X + r.W || y < r.Y || y > r.Y + r.H || r.W <= 0 || r.H <= 0) continue;
            if (!best || r.Depth > best->Depth || (r.Depth == best->Depth && r.W * r.H < best->W * best->H)) best = &r;
        }
        return best ? best->Id : 0;
    };
    uint32_t under = 0;
    if (!m_Try) {
        if (inView) under = rectAt(mx, my);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            m_Selected = m_PendingSelect = under;
            m_DragPanelId = under ? TopPanelOf(under) : 0;
            m_DragPanel = m_DragPanelId != 0;
            m_DragMoved = false;
            m_PressPos = mouse;
        }
        if (m_DragPanel && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            UINode* panel = m_Doc.Find(m_DragPanelId);
            if (panel && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
                bool moved = false;
                for (const char* k : { "x", "y" }) {
                    auto it = panel->Props.find(k);
                    if (it != panel->Props.end() && !it->is_number()) continue;   // an expression: not dragged
                    const float d = k[0] == 'x' ? io.MouseDelta.x / size.x : io.MouseDelta.y / size.y;
                    const float now = (it != panel->Props.end() ? it->get<float>() : 0.5f) + d;
                    panel->Props[k] = std::round(std::clamp(now, -0.5f, 1.5f) * 10000.0f) / 10000.0f;
                    moved = true;
                }
                if (moved) m_DragMoved = true;
            }
        }
        if (m_DragPanel && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (m_DragMoved) Changed();
            m_DragPanel = false;
        }
    }

    // ---- outlines: every panel faintly, the hovered widget, the selected one
    auto outline = [&](uint32_t id, ImU32 color, float thick) {
        for (const UINodeRect& r : m_Rects) {
            if (r.Id != id) continue;
            const ImVec2 a = Lerp(vmin, vmax, r.X, r.Y), b = Lerp(vmin, vmax, r.X + r.W, r.Y + r.H);
            dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), color, 2.0f, 0, thick);
        }
    };
    if (m_Outlines) {
        for (const UINodeRect& r : m_Rects)
            if (r.Depth > 0 && r.W > 0 && r.H > 0)
                dl->AddRect(Lerp(vmin, vmax, r.X, r.Y), Lerp(vmin, vmax, r.X + r.W, r.Y + r.H), IM_COL32(120, 200, 255, 28));
    }
    const uint32_t hot = !m_Try && under ? under : m_Hovered;
    if (hot && hot != m_Selected) outline(hot, IM_COL32(255, 255, 255, 110), 1.0f);
    if (m_Selected) outline(m_Selected, IM_COL32(255, 170, 40, 255), 2.0f);
    dl->PopClipRect();
    dl->AddRect(vmin, vmax, IM_COL32(255, 255, 255, 40));
    dl->PopClipRect();
    m_Hovered = 0;

    // ---- footer: what went wrong, else a hint
    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + avail.y + 4.0f));
    const auto& errors = m_Preview.Errors();
    if (!m_PreviewError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "Layout error: %s", m_PreviewError.c_str());
    else if (!errors.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%zu expression%s failed on the preview values: %s", errors.size(),
            errors.size() == 1 ? "" : "s", errors[0].c_str());
        if (ImGui::BeginItemTooltip()) {
            for (const std::string& e : errors) ImGui::BulletText("%s", e.c_str());
            ImGui::TextDisabled("Give the names they use sample values in the Preview data tab.");
            ImGui::EndTooltip();
        }
    }
    else ImGui::TextDisabled(m_Try ? "Trying it: clicks run on the preview values" :
        "Click a widget to select it, drag to move its panel. Wheel zooms, right drag pans, F frames. %.0f x 720 units", 720.0f * aspect);
}

// ------------------------------------------------------------------ properties
bool UIEditor::PropText(UINode& n, const char* label, const char* key, bool multiline, const char* help) {
    std::string s = n.Props.contains(key) && n.Props[key].is_string() ? n.Props[key].get<std::string>() : std::string();
    ImGui::PushID(key);
    bool changed;
    if (multiline) {
        ImGui::TextUnformatted(label);
        Help(help);
        const float lines = (float)std::clamp((int)std::count(s.begin(), s.end(), '\n') + 2, 3, 10);
        changed = InputStr("##v", s, true, ImGui::GetTextLineHeight() * lines + 8.0f);
    }
    else {
        changed = InputStr(label, s);
        Help(help);
    }
    if (changed) { n.Props[key] = s; Changed(); }
    ImGui::PopID();
    return changed;
}

bool UIEditor::PropExpr(UINode& n, const char* label, const char* key, const char* hint, const char* help) {
    std::string s = n.Props.contains(key) && n.Props[key].is_string() ? n.Props[key].get<std::string>()
                  : n.Props.contains(key) && n.Props[key].is_number() ? n.Props[key].dump() : std::string();
    ImGui::PushID(key);
    const bool changed = InputStr(label, s, false, 0.0f, hint);
    Help(help);
    if (changed) {
        if (s.empty()) n.Props.erase(key);
        else n.Props[key] = s;
        Changed();
    }
    ImGui::PopID();
    return changed;
}

// A fixed number, or (f) a Lua expression giving one
bool UIEditor::PropNumber(UINode& n, const char* label, const char* key, float def, float speed, float lo, float hi, const char* help) {
    auto it = n.Props.find(key);
    const bool expr = it != n.Props.end() && it->is_string();
    ImGui::PushID(key);
    bool changed = false;
    const float w = ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - 4.0f;
    ImGui::SetNextItemWidth(w);
    if (expr) {
        std::string s = it->get<std::string>();
        if (InputStr("##e", s, false, 0.0f, "Lua expression")) { n.Props[key] = s; changed = true; }
    }
    else {
        float v = it != n.Props.end() && it->is_number() ? it->get<float>() : def;
        const char* fmt = speed < 0.01f ? "%.3f" : speed < 0.1f ? "%.2f" : "%.0f";
        if (ImGui::DragFloat("##n", &v, speed, lo, hi, fmt)) { n.Props[key] = v; changed = true; }
    }
    ImGui::SameLine(0, 4);
    if (expr) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.3f, 0.7f, 1.0f));
    if (ImGui::Button("f", ImVec2(ImGui::GetFrameHeight(), 0))) {
        if (expr) n.Props[key] = def;
        else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%g", it != n.Props.end() && it->is_number() ? it->get<float>() : def);
            n.Props[key] = std::string(buf);
        }
        changed = true;
    }
    if (expr) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(expr ? "A Lua expression. Click for a fixed number." : "A fixed number. Click to work it out with a Lua expression.");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    Help(help);
    if (changed) Changed();
    ImGui::PopID();
    return changed;
}

// On / off, or (f) a Lua condition
bool UIEditor::PropBool(UINode& n, const char* label, const char* key, bool def, const char* help) {
    auto it = n.Props.find(key);
    const bool expr = it != n.Props.end() && it->is_string();
    ImGui::PushID(key);
    bool changed = false;
    if (expr) {
        std::string s = it->get<std::string>();
        ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - 4.0f);
        if (InputStr("##e", s, false, 0.0f, "Lua condition")) { n.Props[key] = s; changed = true; }
        ImGui::SameLine(0, 4);
    }
    else {
        bool v = it != n.Props.end() && it->is_boolean() ? it->get<bool>() : def;
        if (ImGui::Checkbox("##b", &v)) { n.Props[key] = v; changed = true; }
        ImGui::SameLine(0, 4);
    }
    if (expr) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.3f, 0.7f, 1.0f));
    if (ImGui::Button("f", ImVec2(ImGui::GetFrameHeight(), 0))) {
        if (expr) n.Props[key] = def;
        else n.Props[key] = std::string(it != n.Props.end() && it->is_boolean() ? (it->get<bool>() ? "true" : "false") : def ? "true" : "false");
        changed = true;
    }
    if (expr) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(expr ? "A Lua condition. Click for a plain on / off." : "On or off. Click to decide it with a Lua condition.");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    Help(help);
    if (changed) Changed();
    ImGui::PopID();
    return changed;
}

bool UIEditor::PropColor(UINode& n, const char* label, const char* key, ImVec4 def) {
    auto it = n.Props.find(key);
    const bool set = it != n.Props.end() && it->is_array() && it->size() >= 3;
    float c[3] = { def.x, def.y, def.z };
    if (set) for (int i = 0; i < 3; i++) c[i] = (*it)[i].get<float>();
    ImGui::PushID(key);
    bool changed = false;
    bool use = set;
    if (ImGui::Checkbox("##use", &use)) {
        if (use) n.Props[key] = { c[0], c[1], c[2] };
        else n.Props.erase(key);
        changed = true;
    }
    ImGui::SetItemTooltip(use ? "Own colour (untick for the theme's)" : "The theme's colour (tick to choose one)");
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(!use);
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - 4.0f);
    if (ImGui::ColorEdit3("##c", c)) { n.Props[key] = { c[0], c[1], c[2] }; changed = true; }
    ImGui::EndDisabled();
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    if (changed) Changed();
    ImGui::PopID();
    return changed;
}

bool UIEditor::PropCombo(UINode& n, const char* label, const char* key, const std::vector<const char*>& options) {
    std::string cur = n.Props.contains(key) && n.Props[key].is_string() ? n.Props[key].get<std::string>() : options[0];
    bool changed = false;
    if (ImGui::BeginCombo(label, cur.c_str())) {
        for (const char* o : options)
            if (ImGui::Selectable(o, cur == o)) { n.Props[key] = o; changed = true; }
        ImGui::EndCombo();
    }
    if (changed) Changed();
    return changed;
}

// An image path (typed, picked from Assets/Textures, or dropped from the Assets panel); may hold {=expr}
bool UIEditor::PropImage(UINode& n, const char* label, const char* key, const char* help) {
    std::string s = n.Props.contains(key) && n.Props[key].is_string() ? n.Props[key].get<std::string>() : std::string();
    ImGui::PushID(key);
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - 4.0f);
    if (InputStr("##p", s, false, 0.0f, "Textures/x.png or {=expr}")) changed = true;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(AssetBrowser::Payload)) {
            const std::string rel = AssetBrowser::PayloadPath(p);
            if (AssetBrowser::KindOf(rel) == AssetBrowser::Kind::Texture) { s = rel; changed = true; }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine(0, 4);
    if (ImGui::Button("...", ImVec2(ImGui::GetFrameHeight(), 0))) ImGui::OpenPopup("##pick");
    ImGui::SetItemTooltip("Pick an image from Assets/Textures");
    if (ImGui::BeginPopup("##pick")) {
        std::error_code ec;
        const fs::path root = fs::u8path(m_AssetDir);
        int shown = 0;
        for (const auto& f : fs::recursive_directory_iterator(root / "Textures", ec)) {
            if (!f.is_regular_file()) continue;
            const std::string rel = fs::relative(f.path(), root, ec).generic_string();
            if (AssetBrowser::KindOf(rel) != AssetBrowser::Kind::Texture) continue;
            const uint32_t tex = TextureId ? TextureId(rel) : 0;
            if (tex) { ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(24, 24)); ImGui::SameLine(); }
            if (ImGui::Selectable(rel.c_str(), rel == s)) { s = rel; changed = true; }
            if (++shown > 400) break;
        }
        if (!shown) ImGui::TextDisabled("No images in Assets/Textures");
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    Help(help);
    if (changed) { n.Props[key] = s; Changed(); }
    ImGui::PopID();
    return changed;
}

void UIEditor::DrawProperties() {
    if (!ImGui::BeginTabBar("##proptabs")) return;
    if (ImGui::BeginTabItem("Widget")) {
        ImGui::BeginChild("##widget");
        if (UINode* n = m_Doc.Find(m_Selected)) DrawWidget(*n);
        else {
            ImGui::TextDisabled("Select a widget in the tree or the preview.");
            ImGui::Spacing();
            ImGui::TextWrapped("A script shows this layout every frame with");
            ImGui::TextColored(ImVec4(0.6f, 0.95f, 0.6f, 1.0f), "  ui.show(\"%s\")", m_Path.empty() ? "UI/<name>.json" : m_Path.c_str());
            ImGui::TextWrapped("It returns the name of the button or slot clicked this frame (and its repeat index), so:");
            ImGui::TextColored(ImVec4(0.6f, 0.95f, 0.6f, 1.0f), "  local clicked = ui.show(...)\n  if clicked == \"resume\" then ... end");
            ImGui::TextWrapped("Texts take {=expr}; most values can be expressions (the f button). They see the script's own variables.");
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Preview data")) {
        DrawPreviewData();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void UIEditor::DrawWidget(UINode& n) {
    const std::string& t = n.Type;
    ImGui::TextColored(TypeColor(t), "%s", TypeLabel(t));
    ImGui::SameLine();
    ImGui::TextDisabled("#%u", n.Id);
    ImGui::PushItemWidth(-150.0f);

    std::string name = n.Name;
    if (InputStr("Name", name)) { n.Name = name; Changed(); }
    Help("What ui.show returns when this button or slot is clicked (or this panel's x). Also tells widgets apart while they are pressed.");
    ImGui::Separator();

    if (t == "panel") {
        PropText(n, "Title", "title", false, "Text at the top, with a line under it. Markup and {=expr} work.");
        PropNumber(n, "X", "x", 0.5f, 0.002f, -0.5f, 1.5f, "Where the anchor sits, 0..1 of the view's width. Drag the panel in the preview to move it.");
        PropNumber(n, "Y", "y", 0.5f, 0.002f, -0.5f, 1.5f, "0..1 of the view's height");
        PropCombo(n, "Anchor", "anchor", { "center", "top", "bottom", "left", "right", "topleft", "topright", "bottomleft", "bottomright" });
        PropNumber(n, "Width", "w", 400.0f, 1.0f, 0.0f, 2000.0f, "Units (pixels at 720p). 0: as wide as its content (text doesn't wrap then).");
        PropNumber(n, "Height", "h", 0.0f, 1.0f, 0.0f, 2000.0f, "0: as tall as its content");
        PropNumber(n, "Padding", "padding", -1.0f, 0.5f, -1.0f, 200.0f, "-1: the theme's");
        PropBool(n, "Close button", "close", false, "An x in the corner. Clicking it makes ui.show return the panel's name and runs On close.");
        PropExpr(n, "On close", "on_close", "Lua, e.g. open = false", "Lua statements run when its x is clicked");
        ImGui::SeparatorText("Look");
        PropColor(n, "Colour", "color", ImVec4(0.055f, 0.065f, 0.09f, 1.0f));
        PropNumber(n, "Alpha", "alpha", -1.0f, 0.01f, -1.0f, 1.0f, "-1: the theme's");
        PropImage(n, "Frame image", "image", "A 9-slice frame instead of the flat panel");
        PropNumber(n, "Slice", "slice", -1.0f, 0.5f, -1.0f, 256.0f, "Texture pixels kept unstretched on each side of the frame image");
    }
    else if (t == "row") PropNumber(n, "Spacing", "spacing", -1.0f, 0.5f, -1.0f, 200.0f, "Between children, side by side. -1: the theme's.");
    else if (t == "column") PropNumber(n, "Width", "width", 0.0f, 1.0f, 0.0f, 2000.0f, "0: the width left. Columns go side by side in a row.");
    else if (t == "scroll") PropNumber(n, "Height", "height", 200.0f, 1.0f, 10.0f, 2000.0f, "Taller content scrolls (wheel or bar)");
    else if (t == "label") {
        PropText(n, "Text", "text", true, "Markup: {#ffcc66}colour{/}, {b}bold{/b}. {=expr} puts in a Lua value, e.g. {=game.gold} gold.");
        PropNumber(n, "Size", "size", 0.0f, 0.5f, 0.0f, 200.0f, "0: the theme's");
        PropColor(n, "Colour", "color", ImVec4(0.93f, 0.94f, 0.96f, 1.0f));
        PropCombo(n, "Align", "align", { "left", "center", "right" });
        PropBool(n, "Bold", "bold", false);
        PropBool(n, "Muted", "muted", false, "The theme's dimmer text colour");
        PropBool(n, "Wrap", "wrap", true);
        PropBool(n, "Shadow", "shadow", false);
        PropNumber(n, "Width", "width", 0.0f, 1.0f, 0.0f, 2000.0f, "0: what the container has left");
    }
    else if (t == "button") {
        PropText(n, "Text", "text", false, "The label. Markup and {=expr} work.");
        PropExpr(n, "On click", "on_click", "Lua, e.g. shared.paused = false", "Lua statements run when it is clicked (in the calling script)");
        PropText(n, "Event", "event", false, "Also sends events.emit(<this>, { ui, widget, index }) to every script's on_event");
        PropBool(n, "Disabled", "disabled", false, "Greyed out and can't be clicked. Use f for a condition, e.g. game.gold < 10");
        PropBool(n, "Selected", "selected", false, "Drawn in the accent colour (a chosen tab or option)");
        PropText(n, "Tooltip", "tooltip", false);
        ImGui::SeparatorText("Size");
        PropNumber(n, "Width", "w", 0.0f, 1.0f, 0.0f, 2000.0f, "0: fits the label");
        PropNumber(n, "Height", "h", 0.0f, 1.0f, 0.0f, 400.0f, "0: the theme's");
        PropBool(n, "Fill", "fill", false, "As wide as the container has left");
        PropNumber(n, "Text size", "size", 0.0f, 0.5f, 0.0f, 200.0f, "0: the theme's");
        PropColor(n, "Colour", "color", ImVec4(0.16f, 0.2f, 0.3f, 1.0f));
    }
    else if (t == "image") {
        PropImage(n, "Image", "path");
        PropNumber(n, "Width", "w", 64.0f, 1.0f, 1.0f, 2000.0f);
        PropNumber(n, "Height", "h", 0.0f, 1.0f, 0.0f, 2000.0f, "0: as tall as it is wide");
        PropColor(n, "Tint", "color", ImVec4(1, 1, 1, 1));
        PropNumber(n, "Alpha", "alpha", 1.0f, 0.01f, 0.0f, 1.0f);
    }
    else if (t == "bar") {
        PropNumber(n, "Value", "value", 0.5f, 0.01f, 0.0f, 1.0f, "How full, 0..1. Usually an expression (f), e.g. hero.npc:stat(\"health\") / 12");
        PropText(n, "Text", "text", false, "Written over the bar, e.g. {=hp} / {=max_hp}");
        PropNumber(n, "Width", "w", 0.0f, 1.0f, 0.0f, 2000.0f, "0: the width left");
        PropNumber(n, "Height", "h", 0.0f, 0.5f, 0.0f, 200.0f, "0: 18");
        PropColor(n, "Colour", "color", ImVec4(0.3f, 0.85f, 0.4f, 1.0f));
        PropColor(n, "Back", "back", ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
    }
    else if (t == "slot") {
        PropImage(n, "Icon", "icon", "Empty: an empty slot. In a repeat, e.g. {=item.icon}");
        PropNumber(n, "Count", "count", 0.0f, 1.0f, 0.0f, 9999.0f, "Shown in the corner when above 1");
        PropText(n, "Tooltip", "tooltip", false);
        PropBool(n, "Highlight", "highlight", false);
        PropExpr(n, "On click", "on_click", "Lua", "Lua statements run when it is clicked");
        PropText(n, "Event", "event", false, "Also sends events.emit(<this>, { ui, widget, index })");
        PropNumber(n, "Size", "size", 0.0f, 1.0f, 0.0f, 400.0f, "0: the theme's");
        PropColor(n, "Colour", "color", ImVec4(0.1f, 0.12f, 0.16f, 1.0f));
    }
    else if (t == "space") PropNumber(n, "Size", "h", 8.0f, 0.5f, 0.0f, 400.0f, "Units of empty space (sideways in a row)");
    else if (t == "separator") ImGui::TextDisabled("A thin line across the container (a gap in a row).");

    ImGui::SeparatorText("Shown");
    PropExpr(n, "Visible", "visible", "always", "A Lua condition: the widget (and what is inside it) is drawn only while it holds, e.g. shared.quest ~= nil");
    PropExpr(n, "Repeat", "repeat", "once",
        "A count or a Lua list: the widget is drawn once per entry, with `index` (1, 2, ...) and `item` (the entry) set for its "
        "expressions. E.g. game.bag in a row of slots, with icon {=item.icon}. Clicks return the name and the index.");
    if (t != "panel" && t != "button" && t != "slot" && t != "space" && t != "separator") PropText(n, "Tooltip", "tooltip", false, "Shown when the pointer rests on it");
    ImGui::PopItemWidth();

    ImGui::Spacing();
    if (ImGui::Button("Duplicate")) m_Later.push_back([this, id = n.Id]() { Duplicate(id); });
    ImGui::SameLine();
    if (ImGui::Button("Delete")) m_Later.push_back([this, id = n.Id]() { Delete(id); });
    for (auto& f : m_Later) f();
    m_Later.clear();
}

void UIEditor::DrawPreviewData() {
    const std::string now = m_Doc.Preview.dump();
    if (now != m_PreviewTextFor && ImGui::GetActiveID() != ImGui::GetID("##json")) {   // loaded / undone: show it again
        m_PreviewTextFor = now;
        m_PreviewText = m_Doc.Preview.empty() ? std::string("{\n}") : m_Doc.Preview.dump(2);
        m_PreviewTextError.clear();
    }
    ImGui::TextWrapped("Sample values for the preview, as JSON: the names the layout's expressions use. They aren't used in the game, where "
                       "the expressions see the script's real variables.");
    ImGui::TextDisabled("e.g. { \"game\": { \"gold\": 120 }, \"hp\": 7, \"items\": [ { \"icon\": \"Textures/Icons/sword.png\" } ] }");
    const float h = std::max(120.0f, ImGui::GetContentRegionAvail().y - 140.0f);
    if (InputStr("##json", m_PreviewText, true, h)) {
        json j = json::parse(m_PreviewText, nullptr, false);
        if (j.is_discarded() || !j.is_object()) m_PreviewTextError = "Not a JSON object yet (it is used once it parses)";
        else {
            m_PreviewTextError.clear();
            m_Doc.Preview = j;
            m_PreviewTextFor = j.dump();
            Changed();
        }
    }
    if (!m_PreviewTextError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", m_PreviewTextError.c_str());
    if (ImGui::Button("Reset values")) m_Preview.Reset();
    ImGui::SameLine();
    ImGui::TextDisabled("Try it may have changed them");
    if (!m_Preview.Ran.empty()) {
        ImGui::SeparatorText("Clicked (Try it)");
        for (auto it = m_Preview.Ran.rbegin(); it != m_Preview.Ran.rend() && it - m_Preview.Ran.rbegin() < 5; ++it)
            ImGui::TextDisabled("%s", it->c_str());
    }
}

// ------------------------------------------------------------------ dialogs
void UIEditor::DrawPopups() {
    if (m_AskUnsaved) { ImGui::OpenPopup("Unsaved UI layout"); m_AskUnsaved = false; }
    if (ImGui::BeginPopupModal("Unsaved UI layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has unsaved changes.", m_Path.empty() ? "This layout" : m_Path.c_str());
        auto proceed = [&]() {
            m_SavedText = Text();
            if (m_PendingNew) New(); else Load(m_PendingOpen);
            ImGui::CloseCurrentPopup();
        };
        if (!m_Path.empty() && ImGui::Button("Save", ImVec2(100, 0))) { if (SaveTo(m_Path)) proceed(); }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100, 0))) proceed();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_AskSaveAs) {
        ImGui::OpenPopup("Save UI layout as");
        m_AskSaveAs = false;
        const std::string stem = m_Path.empty() ? std::string("new_layout") : fs::path(m_Path).stem().string();
        std::snprintf(m_SaveAsName, sizeof(m_SaveAsName), "%s", stem.c_str());
    }
    if (ImGui::BeginPopupModal("Save UI layout as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Assets/UI/");
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText(".json", m_SaveAsName, sizeof(m_SaveAsName), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("A script draws it with ui.show(\"UI/%s.json\")", m_SaveAsName);
        if ((ImGui::Button("Save", ImVec2(100, 0)) || enter) && m_SaveAsName[0]) {
            SaveTo(std::string("UI/") + m_SaveAsName + ".json");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
