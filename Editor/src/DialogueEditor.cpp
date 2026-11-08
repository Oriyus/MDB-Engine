#include "DialogueEditor.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
    // Sizes in canvas units (pixels at zoom 1)
    constexpr float kNodeW = 270.0f, kHeaderH = 26.0f, kRowH = 24.0f, kPinR = 6.0f;

    ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
    ImVec2 Sub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
    ImVec2 Mul(ImVec2 a, float s) { return ImVec2(a.x * s, a.y * s); }
    bool Inside(ImVec2 p, ImVec2 mn, ImVec2 mx) { return p.x >= mn.x && p.y >= mn.y && p.x <= mx.x && p.y <= mx.y; }

    ImU32 HeaderColor(const std::string& type) {
        if (type == "branch") return IM_COL32(118, 72, 150, 255);
        if (type == "action") return IM_COL32(168, 108, 40, 255);
        return IM_COL32(52, 88, 150, 255);
    }
    const char* TypeName(const std::string& type) {
        return type == "branch" ? "Branch" : type == "action" ? "Action" : "Line";
    }

    int ResizeCallback(ImGuiInputTextCallbackData* d) {
        if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* s = static_cast<std::string*>(d->UserData);
            s->resize(d->BufTextLen);
            d->Buf = s->data();
        }
        return 0;
    }

    // The text without its style tags ({#rrggbb}, {/}, {b}, {/b}), for previews; {=expr} stays
    std::string StripMarkup(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '{' && i + 1 < s.size() && s[i + 1] != '=') {
                const size_t end = s.find('}', i);
                if (end != std::string::npos && end - i <= 8) { i = end; continue; }
            }
            out += s[i];
        }
        return out;
    }

    std::string FirstLine(const std::string& s) {
        const size_t n = s.find('\n');
        return n == std::string::npos ? s : s.substr(0, n) + " ...";
    }
}

DialogueEditor::DialogueEditor(std::string assetDir) : m_AssetDir(std::move(assetDir)) {
    New();
}

// ------------------------------------------------------------------ file
json DialogueEditor::ToJson() const {
    json nodes = json::array();
    for (const Node& n : m_Nodes) {
        json j = { { "id", n.Id }, { "type", n.Type }, { "pos", { std::round(n.Pos.x), std::round(n.Pos.y) } } };
        if (n.Type == "branch") {
            j["cond"] = n.Cond;
            j["next"] = n.Next;
            j["else"] = n.Else;
        }
        else if (n.Type == "action") {
            j["action"] = n.Action;
            j["event"] = n.Event;
            j["next"] = n.Next;
        }
        else {
            j["speaker"] = n.Speaker;
            j["text"] = n.Text;
            j["action"] = n.Action;
            j["event"] = n.Event;
            j["next"] = n.Next;
            json choices = json::array();
            for (const Choice& c : n.Choices)
                choices.push_back({ { "text", c.Text }, { "cond", c.Cond }, { "show", c.Show }, { "why", c.Why },
                                    { "action", c.Action }, { "event", c.Event }, { "next", c.Next } });
            j["choices"] = choices;
        }
        nodes.push_back(std::move(j));
    }
    return { { "version", 1 }, { "start", m_Start }, { "nodes", nodes } };
}

void DialogueEditor::FromJson(const json& j) {
    m_Nodes.clear();
    m_Start = j.value("start", 1);
    for (const json& jn : j.value("nodes", json::array())) {
        Node n;
        n.Id = jn.value("id", 0);
        n.Type = jn.value("type", std::string("line"));
        n.Speaker = jn.value("speaker", std::string());
        n.Text = jn.value("text", std::string());
        n.Cond = jn.value("cond", std::string());
        n.Action = jn.value("action", std::string());
        n.Event = jn.value("event", std::string());
        n.Next = jn.value("next", 0);
        n.Else = jn.value("else", 0);
        if (jn.contains("pos") && jn["pos"].is_array() && jn["pos"].size() == 2)
            n.Pos = ImVec2(jn["pos"][0].get<float>(), jn["pos"][1].get<float>());
        for (const json& jc : jn.value("choices", json::array())) {
            Choice c;
            c.Text = jc.value("text", std::string());
            c.Cond = jc.value("cond", std::string());
            c.Show = jc.value("show", std::string("hidden"));
            c.Why = jc.value("why", std::string());
            c.Action = jc.value("action", std::string());
            c.Event = jc.value("event", std::string());
            c.Next = jc.value("next", 0);
            n.Choices.push_back(std::move(c));
        }
        if (n.Id > 0) m_Nodes.push_back(std::move(n));
    }
}

bool DialogueEditor::Load(const std::string& path) {
    std::ifstream in(m_AssetDir + path);
    if (!in) { m_Status = "Can't open " + path; return false; }
    try {
        json j;
        in >> j;
        FromJson(j);
    }
    catch (const std::exception& ex) {
        m_Status = path + " is not a dialogue: " + ex.what();
        return false;
    }
    m_Path = path;
    m_SavedText = ToJson().dump();
    m_Selected = 0;
    ResetHistory();
    m_FrameNext = true;
    m_Status = "Opened " + path;
    return true;
}

bool DialogueEditor::SaveTo(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(m_AssetDir + path).parent_path(), ec);
    std::ofstream out(m_AssetDir + path);
    if (!out) { m_Status = "Can't write " + path; return false; }
    out << ToJson().dump(2);
    if (!out) { m_Status = "Can't write " + path; return false; }
    m_Path = path;
    m_SavedText = ToJson().dump();
    m_Status = "Saved " + path;
    return true;
}

bool DialogueEditor::SaveIfNamed() { return !m_Path.empty() && SaveTo(m_Path); }

bool DialogueEditor::IsDirty() const { return ToJson().dump() != m_SavedText; }

std::vector<std::string> DialogueEditor::ListFiles() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(m_AssetDir + "Dialogues", ec))
        if (f.path().extension() == ".json") out.push_back("Dialogues/" + f.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

void DialogueEditor::New() {
    if (!m_Nodes.empty() && IsDirty()) { m_PendingNew = true; m_PendingOpen.clear(); m_AskUnsaved = true; return; }
    m_Nodes.clear();
    m_Path.clear();
    Node& first = AddNode("line", ImVec2(60, 120));
    first.Text = "Hello there.";
    m_Start = first.Id;
    m_SavedText = ToJson().dump();
    m_Selected = 0;
    ResetHistory();
    m_FrameNext = true;
    m_Status = "New dialogue";
}

void DialogueEditor::Open(const std::string& path) {
    if (!m_Nodes.empty() && IsDirty() && path != m_Path) { m_PendingOpen = path; m_PendingNew = false; m_AskUnsaved = true; return; }
    Load(path);
}

// ------------------------------------------------------------------ graph
DialogueEditor::Node* DialogueEditor::Find(int id) {
    for (Node& n : m_Nodes) if (n.Id == id) return &n;
    return nullptr;
}

int DialogueEditor::NewId() const {
    int id = 0;
    for (const Node& n : m_Nodes) id = std::max(id, n.Id);
    return id + 1;
}

DialogueEditor::Node& DialogueEditor::AddNode(const std::string& type, ImVec2 pos) {
    Node n;
    n.Id = NewId();
    n.Type = type;
    n.Pos = pos;
    if (type == "line") n.Text = "...";
    m_Nodes.push_back(std::move(n));
    return m_Nodes.back();
}

void DialogueEditor::Remove(int id) {
    m_Nodes.erase(std::remove_if(m_Nodes.begin(), m_Nodes.end(), [&](const Node& n) { return n.Id == id; }), m_Nodes.end());
    for (Node& n : m_Nodes) {                  // links to it now end the conversation
        if (n.Next == id) n.Next = 0;
        if (n.Else == id) n.Else = 0;
        for (Choice& c : n.Choices) if (c.Next == id) c.Next = 0;
    }
    if (m_Selected == id) m_Selected = 0;
}

int DialogueEditor::PinCount(const Node& n) const {
    if (n.Type == "branch") return 2;
    if (n.Type == "line" && !n.Choices.empty()) return (int)n.Choices.size();
    return 1;
}

int* DialogueEditor::Target(Node& n, int pin) {
    if (n.Type == "branch") return pin == 1 ? &n.Else : &n.Next;
    if (n.Type == "line" && !n.Choices.empty()) return pin >= 0 && pin < (int)n.Choices.size() ? &n.Choices[pin].Next : nullptr;
    return &n.Next;
}

// Problems worth fixing; `nodes` gets the node each one is about (0 = the whole dialogue)
std::vector<std::string> DialogueEditor::Issues(std::vector<int>* nodes) {
    std::vector<std::string> out;
    auto add = [&](int id, const std::string& s) { out.push_back(s); if (nodes) nodes->push_back(id); };
    if (!Find(m_Start)) add(0, "The start node #" + std::to_string(m_Start) + " doesn't exist");
    for (Node& n : m_Nodes) {
        for (int p = 0; p < PinCount(n); p++) {
            const int* t = Target(n, p);
            if (t && *t != 0 && !Find(*t)) add(n.Id, "#" + std::to_string(n.Id) + " links to #" + std::to_string(*t) + ", which doesn't exist");
        }
        if (n.Type == "line" && n.Text.empty()) add(n.Id, "#" + std::to_string(n.Id) + " has no text");
        if (n.Type == "branch" && n.Cond.empty()) add(n.Id, "#" + std::to_string(n.Id) + " (branch) has no condition");
        for (size_t i = 0; i < n.Choices.size(); i++)
            if (n.Choices[i].Text.empty()) add(n.Id, "#" + std::to_string(n.Id) + " choice " + std::to_string(i + 1) + " has no text");
    }
    // unreachable from the start
    std::unordered_set<int> seen;
    std::vector<int> todo{ m_Start };
    while (!todo.empty()) {
        const int id = todo.back();
        todo.pop_back();
        Node* n = Find(id);
        if (!n || !seen.insert(id).second) continue;
        for (int p = 0; p < PinCount(*n); p++) if (const int* t = Target(*n, p); t && *t) todo.push_back(*t);
    }
    for (const Node& n : m_Nodes)
        if (!seen.contains(n.Id)) add(n.Id, "#" + std::to_string(n.Id) + " can't be reached from the start");
    return out;
}

// ------------------------------------------------------------------ undo
void DialogueEditor::ResetHistory() {
    m_History.assign(1, ToJson().dump());
    m_HistoryPos = 0;
}

void DialogueEditor::Commit() {
    const std::string s = ToJson().dump();
    if (m_HistoryPos >= 0 && m_History[m_HistoryPos] == s) return;
    m_History.resize(m_HistoryPos + 1);
    m_History.push_back(s);
    if (m_History.size() > 200) m_History.erase(m_History.begin());
    m_HistoryPos = (int)m_History.size() - 1;
}

void DialogueEditor::Undo() {
    Commit();                                  // an edit still being typed counts as a step
    if (m_HistoryPos <= 0) return;
    FromJson(json::parse(m_History[--m_HistoryPos]));
    if (!Find(m_Selected)) m_Selected = 0;
}

void DialogueEditor::Redo() {
    if (m_HistoryPos + 1 >= (int)m_History.size()) return;
    FromJson(json::parse(m_History[++m_HistoryPos]));
    if (!Find(m_Selected)) m_Selected = 0;
}

// ------------------------------------------------------------------ view helpers
ImVec2 DialogueEditor::ToScreen(ImVec2 c) const { return Add(Add(m_CanvasMin, m_Pan), Mul(c, m_Zoom)); }
ImVec2 DialogueEditor::ToCanvas(ImVec2 s) const { return Mul(Sub(Sub(s, m_CanvasMin), m_Pan), 1.0f / m_Zoom); }

DialogueEditor::Geo DialogueEditor::Layout(const Node& n) const {
    Geo g;
    const float z = m_Zoom, line = ImGui::GetFontSize() * 1.2f;
    const float body = (n.Type == "line" ? 3 * line : n.Type == "action" ? 2 * line : line) + 10.0f;
    std::vector<std::string> labels;
    if (n.Type == "branch") labels = { "True", "False" };
    else if (n.Type == "action") labels = { "Next" };
    else if (n.Choices.empty()) labels = { n.Next ? "Continue" : "Continue (ends here)" };
    else for (const Choice& c : n.Choices) labels.push_back((c.Cond.empty() ? "" : "(if) ") + FirstLine(c.Text));
    g.Labels = labels;
    const float h = kHeaderH + body + labels.size() * kRowH + 6.0f;
    g.Min = ToScreen(n.Pos);
    g.Max = Add(g.Min, ImVec2(kNodeW * z, h * z));
    g.In = Add(g.Min, ImVec2(0, kHeaderH * 0.5f * z));
    g.BodyTop = g.Min.y + kHeaderH * z;
    g.BodyBottom = g.BodyTop + body * z;
    for (size_t i = 0; i < labels.size(); i++)
        g.Outs.push_back(ImVec2(g.Max.x, g.BodyBottom + (i * kRowH + kRowH * 0.5f) * z));
    return g;
}

int DialogueEditor::NodeAt(ImVec2 p) const {
    // the selected node is drawn on top, so it is hit first
    for (const Node& n : m_Nodes)
        if (n.Id == m_Selected) { Geo g = Layout(n); if (Inside(p, g.Min, g.Max)) return n.Id; }
    for (auto it = m_Nodes.rbegin(); it != m_Nodes.rend(); ++it) {
        Geo g = Layout(*it);
        if (Inside(p, g.Min, g.Max)) return it->Id;
    }
    return 0;
}

bool DialogueEditor::PinAt(ImVec2 p, int& node, int& pin) const {
    const float r = (kPinR + 4.0f) * m_Zoom;
    for (const Node& n : m_Nodes) {
        Geo g = Layout(n);
        for (size_t i = 0; i < g.Outs.size(); i++) {
            const ImVec2 d = Sub(p, g.Outs[i]);
            if (d.x * d.x + d.y * d.y <= r * r) { node = n.Id; pin = (int)i; return true; }
        }
    }
    return false;
}

void DialogueEditor::FrameAll() {
    if (m_Nodes.empty() || m_CanvasSize.x <= 0) return;
    ImVec2 mn(FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX);
    for (const Node& n : m_Nodes) {
        mn = ImVec2(std::min(mn.x, n.Pos.x), std::min(mn.y, n.Pos.y));
        mx = ImVec2(std::max(mx.x, n.Pos.x + kNodeW), std::max(mx.y, n.Pos.y + 200.0f));
    }
    const float zx = (m_CanvasSize.x - 80) / std::max(1.0f, mx.x - mn.x), zy = (m_CanvasSize.y - 80) / std::max(1.0f, mx.y - mn.y);
    m_Zoom = std::clamp(std::min(zx, zy), 0.2f, 1.2f);
    m_Pan = Sub(ImVec2(40, 40), Mul(mn, m_Zoom));
}

bool DialogueEditor::InputString(const char* label, std::string& s, bool multiline, float height) {
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    const bool changed = multiline
        ? ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, ImVec2(-1, height), flags, ResizeCallback, &s)
        : ImGui::InputText(label, s.data(), s.capacity() + 1, flags, ResizeCallback, &s);
    if (ImGui::IsItemDeactivatedAfterEdit()) m_WantCommit = true;
    return changed;
}

// ------------------------------------------------------------------ the window
void DialogueEditor::Draw(bool* open) {
    const std::string name = m_Path.empty() ? std::string("(unsaved)") : fs::path(m_Path).filename().string();
    const std::string title = "Dialogue Editor - " + name + (IsDirty() ? " *" : "") + "###DialogueEditor";
    ImGui::SetNextWindowSize(ImVec2(1150, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), open, ImGuiWindowFlags_MenuBar)) {
        m_Focused = false;
        ImGui::End();
        return;
    }
    m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // shortcuts while this window has the focus
    const ImGuiIO& io = ImGui::GetIO();
    if (m_Focused) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) { if (m_Path.empty()) m_AskSaveAs = true; else SaveTo(m_Path); }
        if (!io.WantTextInput) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) Redo(); else Undo(); }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && Find(m_Selected)) {
                Node copy = *Find(m_Selected);
                copy.Id = NewId();
                copy.Pos = Add(copy.Pos, ImVec2(30, 30));
                m_Nodes.push_back(copy);
                m_Selected = copy.Id;
                Commit();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && m_Selected) { Remove(m_Selected); Commit(); }
            if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) FrameAll();
        }
    }

    DrawMenuBar();

    const float propsW = 360.0f;
    ImGui::BeginChild("##canvas", ImVec2(ImGui::GetContentRegionAvail().x - propsW - 6.0f, 0), ImGuiChildFlags_Border,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    DrawCanvas();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##props", ImVec2(0, 0), ImGuiChildFlags_Border);
    DrawProperties();
    ImGui::EndChild();

    DrawPopups();
    if (m_WantCommit && !ImGui::IsAnyItemActive()) { Commit(); m_WantCommit = false; }
    ImGui::End();
}

void DialogueEditor::DrawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New")) New();
        if (ImGui::BeginMenu("Open")) {
            const auto files = ListFiles();
            if (files.empty()) ImGui::TextDisabled("No dialogues in Assets/Dialogues yet");
            for (const std::string& f : files)
                if (ImGui::MenuItem(fs::path(f).filename().string().c_str(), nullptr, f == m_Path)) Open(f);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) { if (m_Path.empty()) m_AskSaveAs = true; else SaveTo(m_Path); }
        if (ImGui::MenuItem("Save As...")) m_AskSaveAs = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_HistoryPos > 0)) Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_HistoryPos + 1 < (int)m_History.size())) Redo();
        ImGui::Separator();
        const ImVec2 at = ToCanvas(Add(m_CanvasMin, ImVec2(m_CanvasSize.x * 0.4f, m_CanvasSize.y * 0.4f)));
        if (ImGui::MenuItem("Add Line")) { m_Selected = AddNode("line", at).Id; Commit(); }
        if (ImGui::MenuItem("Add Branch")) { m_Selected = AddNode("branch", at).Id; Commit(); }
        if (ImGui::MenuItem("Add Action")) { m_Selected = AddNode("action", at).Id; Commit(); }
        if (ImGui::MenuItem("Delete Node", "Delete", false, m_Selected != 0)) { Remove(m_Selected); Commit(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Frame All", "F")) FrameAll();
        if (ImGui::MenuItem("Reset Zoom")) m_Zoom = 1.0f;
        ImGui::EndMenu();
    }
    std::vector<int> where;
    const auto issues = Issues(&where);
    ImGui::Separator();
    if (issues.empty()) ImGui::TextDisabled("%zu nodes, no problems", m_Nodes.size());
    else ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%zu nodes, %zu problem%s", m_Nodes.size(), issues.size(), issues.size() == 1 ? "" : "s");
    if (!m_Status.empty()) { ImGui::Separator(); ImGui::TextDisabled("%s", m_Status.c_str()); }
    ImGui::EndMenuBar();
}

void DialogueEditor::DrawCanvas() {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    m_CanvasMin = ImGui::GetCursorScreenPos();
    m_CanvasSize = ImGui::GetContentRegionAvail();
    if (m_CanvasSize.x < 50 || m_CanvasSize.y < 50) return;
    // frame a newly opened dialogue once the canvas has its final size (a docked window settles a frame later)
    if (m_FrameNext && m_CanvasSize.x == m_LastCanvasSize.x && m_CanvasSize.y == m_LastCanvasSize.y) { FrameAll(); m_FrameNext = false; }
    m_LastCanvasSize = m_CanvasSize;
    const ImVec2 canvasMax = Add(m_CanvasMin, m_CanvasSize);
    ImGui::InvisibleButton("##canvas_input", m_CanvasSize,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;

    // ---- view: wheel zooms around the pointer, right / middle drag pans
    if (hovered && io.MouseWheel != 0.0f) {
        const float old = m_Zoom;
        m_Zoom = std::clamp(m_Zoom * (io.MouseWheel > 0 ? 1.12f : 1.0f / 1.12f), 0.2f, 2.0f);
        const ImVec2 rel = Sub(Sub(mouse, m_CanvasMin), m_Pan);
        m_Pan = Sub(Sub(mouse, m_CanvasMin), Mul(rel, m_Zoom / old));
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        m_Pan = Add(m_Pan, io.MouseDelta);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) m_RightDragged = true;
    }

    // ---- left button: link from a pin, move a node, or select nothing
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int node, pin;
        if (PinAt(mouse, node, pin)) { m_LinkNode = node; m_LinkPin = pin; }
        else if (int id = NodeAt(mouse)) { m_Selected = id; m_DragNode = id; m_DragMoved = false; }
        else m_Selected = 0;
    }
    if (m_DragNode && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (Node* n = Find(m_DragNode); n && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            n->Pos = Add(n->Pos, Mul(io.MouseDelta, 1.0f / m_Zoom));
            m_DragMoved = true;
        }
    }
    if (m_DragNode && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_DragMoved) Commit();
        m_DragNode = 0;
    }
    if (m_LinkNode && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        Node* from = Find(m_LinkNode);
        if (const int target = NodeAt(mouse); from && target) {
            if (int* t = Target(*from, m_LinkPin)) { *t = target; Commit(); }
        }
        else if (from && Inside(mouse, m_CanvasMin, canvasMax)) {   // dropped on nothing: make the node it points to
            m_PendingNode = m_LinkNode;
            m_PendingPin = m_LinkPin;
            m_PopupPos = ToCanvas(mouse);
            ImGui::OpenPopup("##newnode");
        }
        m_LinkNode = 0;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !NodeAt(mouse)) {
        int node, pin;
        if (!PinAt(mouse, node, pin)) { m_Selected = AddNode("line", ToCanvas(mouse)).Id; Commit(); }
    }

    // ---- right click (not a drag): unlink a pin, the node menu, or the add menu
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !m_RightDragged) {
        int node, pin;
        if (PinAt(mouse, node, pin)) {
            if (Node* n = Find(node)) if (int* t = Target(*n, pin)) { *t = 0; Commit(); }
        }
        else if (int id = NodeAt(mouse)) { m_Selected = id; m_ContextNode = id; ImGui::OpenPopup("##nodemenu"); }
        else { m_PendingNode = 0; m_PopupPos = ToCanvas(mouse); ImGui::OpenPopup("##newnode"); }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_RightDragged = false;

    // ---- draw
    dl->PushClipRect(m_CanvasMin, canvasMax, true);
    dl->AddRectFilled(m_CanvasMin, canvasMax, IM_COL32(24, 26, 31, 255));
    const float grid = 32.0f * m_Zoom;
    for (float x = std::fmod(m_Pan.x, grid); x < m_CanvasSize.x; x += grid)
        dl->AddLine(ImVec2(m_CanvasMin.x + x, m_CanvasMin.y), ImVec2(m_CanvasMin.x + x, canvasMax.y), IM_COL32(255, 255, 255, 10));
    for (float y = std::fmod(m_Pan.y, grid); y < m_CanvasSize.y; y += grid)
        dl->AddLine(ImVec2(m_CanvasMin.x, m_CanvasMin.y + y), ImVec2(canvasMax.x, m_CanvasMin.y + y), IM_COL32(255, 255, 255, 10));

    std::unordered_map<int, Geo> geo;
    for (const Node& n : m_Nodes) geo[n.Id] = Layout(n);
    std::unordered_set<int> linkedTo;

    auto curve = [&](ImVec2 a, ImVec2 b, ImU32 col, float thick) {
        const float d = std::max(40.0f * m_Zoom, std::abs(b.x - a.x) * 0.5f);
        dl->AddBezierCubic(a, ImVec2(a.x + d, a.y), ImVec2(b.x - d, b.y), b, col, thick * m_Zoom);
    };
    // links
    for (Node& n : m_Nodes) {
        const Geo& g = geo[n.Id];
        for (int p = 0; p < PinCount(n); p++) {
            const int* t = Target(n, p);
            if (!t || !*t || !geo.contains(*t)) continue;
            linkedTo.insert(*t);
            ImU32 col = IM_COL32(170, 200, 255, 200);
            if (n.Type == "branch") col = p == 0 ? IM_COL32(120, 220, 140, 210) : IM_COL32(240, 120, 110, 210);
            else if (n.Type == "action") col = IM_COL32(240, 180, 90, 210);
            else if (n.Choices.empty()) col = IM_COL32(220, 220, 225, 190);
            const bool hot = n.Id == m_Selected || *t == m_Selected;
            curve(g.Outs[p], geo[*t].In, hot ? IM_COL32(255, 210, 100, 255) : col, hot ? 3.0f : 2.2f);
        }
    }

    // nodes (the selected one last, on top)
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * m_Zoom;
    auto drawNode = [&](Node& n) {
        const Geo& g = geo[n.Id];
        const float r = 6.0f * m_Zoom;
        dl->AddRectFilled(Add(g.Min, ImVec2(3, 4)), Add(g.Max, ImVec2(3, 4)), IM_COL32(0, 0, 0, 70), r);   // shadow
        dl->AddRectFilled(g.Min, g.Max, IM_COL32(36, 39, 47, 245), r);
        dl->AddRectFilled(g.Min, ImVec2(g.Max.x, g.BodyTop), HeaderColor(n.Type), r, ImDrawFlags_RoundCornersTop);
        std::string head = "#" + std::to_string(n.Id) + "  " + TypeName(n.Type);
        if (n.Type == "line" && !n.Speaker.empty()) head = "#" + std::to_string(n.Id) + "  " + n.Speaker;
        dl->PushClipRect(g.Min, g.Max, true);
        dl->AddText(font, fs, ImVec2(g.Min.x + 8 * m_Zoom, g.Min.y + (kHeaderH * m_Zoom - fs) * 0.5f), IM_COL32(255, 255, 255, 240), head.c_str());
        if (n.Id == m_Start) {
            const char* tag = "START";
            const ImVec2 ts = font->CalcTextSizeA(fs * 0.8f, FLT_MAX, 0, tag);
            const ImVec2 p0(g.Max.x - ts.x - 14 * m_Zoom, g.Min.y + 5 * m_Zoom);
            dl->AddRectFilled(p0, Add(p0, ImVec2(ts.x + 8 * m_Zoom, kHeaderH * m_Zoom - 10 * m_Zoom)), IM_COL32(60, 170, 90, 255), 4 * m_Zoom);
            dl->AddText(font, fs * 0.8f, Add(p0, ImVec2(4 * m_Zoom, 2 * m_Zoom)), IM_COL32(255, 255, 255, 255), tag);
        }
        // body
        std::string body;
        ImU32 bodyCol = IM_COL32(220, 222, 228, 230);
        if (n.Type == "line") body = StripMarkup(n.Text);
        else if (n.Type == "branch") { body = n.Cond.empty() ? "(no condition)" : "if " + n.Cond; bodyCol = IM_COL32(210, 190, 240, 230); }
        else {
            body = n.Action.empty() ? "" : FirstLine(n.Action);
            if (!n.Event.empty()) body += (body.empty() ? "" : "\n") + std::string("event: ") + n.Event;
            if (body.empty()) body = "(does nothing)";
            bodyCol = IM_COL32(240, 210, 160, 230);
        }
        dl->PushClipRect(ImVec2(g.Min.x, g.BodyTop), ImVec2(g.Max.x, g.BodyBottom), true);
        dl->AddText(font, fs, ImVec2(g.Min.x + 8 * m_Zoom, g.BodyTop + 5 * m_Zoom), bodyCol, body.c_str(), nullptr, (kNodeW - 16) * m_Zoom);
        dl->PopClipRect();
        // rows and output pins
        for (size_t i = 0; i < g.Outs.size(); i++) {
            const float y = g.BodyBottom + i * kRowH * m_Zoom;
            dl->AddLine(ImVec2(g.Min.x + 6 * m_Zoom, y), ImVec2(g.Max.x - 6 * m_Zoom, y), IM_COL32(255, 255, 255, 18));
            ImU32 col = IM_COL32(200, 205, 215, 230);
            if (n.Type == "branch") col = i == 0 ? IM_COL32(130, 220, 150, 255) : IM_COL32(240, 130, 120, 255);
            dl->AddText(font, fs * 0.92f, ImVec2(g.Min.x + 10 * m_Zoom, y + (kRowH * m_Zoom - fs * 0.92f) * 0.5f), col, g.Labels[i].c_str());
        }
        dl->PopClipRect();
        for (int p = 0; p < (int)g.Outs.size(); p++) {
            const int* t = Target(n, p);
            const bool linked = t && *t;
            dl->AddCircleFilled(g.Outs[p], kPinR * m_Zoom, linked ? IM_COL32(170, 200, 255, 255) : IM_COL32(60, 64, 76, 255));
            dl->AddCircle(g.Outs[p], kPinR * m_Zoom, IM_COL32(220, 225, 235, 255), 0, 1.5f);
        }
        dl->AddCircleFilled(g.In, kPinR * m_Zoom, linkedTo.contains(n.Id) || n.Id == m_Start ? IM_COL32(170, 200, 255, 255) : IM_COL32(60, 64, 76, 255));
        dl->AddCircle(g.In, kPinR * m_Zoom, IM_COL32(220, 225, 235, 255), 0, 1.5f);
        const bool selected = n.Id == m_Selected;
        dl->AddRect(g.Min, g.Max, selected ? IM_COL32(255, 200, 80, 255) : n.Id == m_Start ? IM_COL32(80, 190, 110, 255) : IM_COL32(255, 255, 255, 30),
            r, 0, (selected ? 2.5f : 1.5f));
    };
    for (Node& n : m_Nodes) if (n.Id != m_Selected) drawNode(n);
    if (Node* s = Find(m_Selected)) drawNode(*s);

    // the link being dragged
    if (m_LinkNode) if (geo.contains(m_LinkNode) && m_LinkPin < (int)geo[m_LinkNode].Outs.size())
        curve(geo[m_LinkNode].Outs[m_LinkPin], mouse, IM_COL32(255, 210, 100, 255), 2.5f);

    if (m_Nodes.empty())
        dl->AddText(Add(m_CanvasMin, ImVec2(20, 20)), IM_COL32(255, 255, 255, 120), "Double-click to add a line.");
    dl->AddText(ImVec2(m_CanvasMin.x + 8, canvasMax.y - 22), IM_COL32(255, 255, 255, 80),
        "Drag a pin onto a node to link (right-click a pin: unlink).  Right-click: add.  Wheel: zoom.  Right / middle drag: pan.  F: frame all");
    dl->PopClipRect();
    DrawNodePopups();                        // opened above, so drawn in this child window too
}

void DialogueEditor::DrawNodePopups() {
    // a new node, possibly linked from the pin a link was dragged out of
    if (ImGui::BeginPopup("##newnode")) {
        ImGui::TextDisabled(m_PendingNode ? "New node, linked" : "Add node");
        const char* types[] = { "line", "branch", "action" };
        const char* labels[] = { "Line (someone speaks)", "Branch (if / else)", "Action (code, event)" };
        for (int i = 0; i < 3; i++) {
            if (ImGui::MenuItem(labels[i])) {
                const int id = AddNode(types[i], m_PopupPos).Id;
                if (Node* from = Find(m_PendingNode)) if (int* t = Target(*from, m_PendingPin)) *t = id;
                m_Selected = id;
                m_PendingNode = 0;
                Commit();
            }
        }
        ImGui::EndPopup();
    }
    else m_PendingNode = 0;

    if (ImGui::BeginPopup("##nodemenu")) {
        Node* n = Find(m_ContextNode);
        if (n) {
            ImGui::TextDisabled("#%d %s", n->Id, TypeName(n->Type));
            if (ImGui::MenuItem("Start Here", nullptr, m_Start == n->Id)) { m_Start = n->Id; Commit(); }
            if (n->Type == "line" && ImGui::MenuItem("Add Choice")) { n->Choices.push_back(Choice{ "...", "", "hidden", "", "", "", 0 }); Commit(); }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                Node copy = *n;
                copy.Id = NewId();
                copy.Pos = Add(copy.Pos, ImVec2(30, 30));
                m_Nodes.push_back(copy);
                m_Selected = copy.Id;
                Commit();
            }
            if (ImGui::MenuItem("Delete", "Delete")) { Remove(m_ContextNode); Commit(); }
        }
        ImGui::EndPopup();
    }
}

// Modal dialogs opened from the menu bar or by Open / New (drawn in the window itself)
void DialogueEditor::DrawPopups() {
    // unsaved changes before New / Open
    if (m_AskUnsaved) { ImGui::OpenPopup("Unsaved dialogue"); m_AskUnsaved = false; }
    if (ImGui::BeginPopupModal("Unsaved dialogue", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has unsaved changes.", m_Path.empty() ? "This dialogue" : m_Path.c_str());
        auto proceed = [&]() {
            m_SavedText = ToJson().dump();      // so New / Open don't ask again
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
        ImGui::OpenPopup("Save dialogue as");
        m_AskSaveAs = false;
        const std::string stem = m_Path.empty() ? std::string("new_dialogue") : fs::path(m_Path).stem().string();
        std::snprintf(m_SaveAsName, sizeof(m_SaveAsName), "%s", stem.c_str());
    }
    if (ImGui::BeginPopupModal("Save dialogue as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Assets/Dialogues/");
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText(".json", m_SaveAsName, sizeof(m_SaveAsName), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("An entity running npc.lua plays Dialogues/<its name in lower case>.json");
        if ((ImGui::Button("Save", ImVec2(100, 0)) || enter) && m_SaveAsName[0]) {
            SaveTo(std::string("Dialogues/") + m_SaveAsName + ".json");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void DialogueEditor::DrawProperties() {
    Node* n = Find(m_Selected);
    auto targetLabel = [&](int id) -> std::string {
        if (id == 0) return "(ends the conversation)";
        Node* t = Find(id);
        if (!t) return "#" + std::to_string(id) + " (missing)";
        std::string s = "#" + std::to_string(id) + " " + TypeName(t->Type);
        if (t->Type == "line") s += ": " + FirstLine(t->Text).substr(0, 40);
        return s;
    };
    auto targetField = [&](const char* label, int& next) {
        ImGui::SetNextItemWidth(90);
        if (ImGui::InputInt(label, &next, 0)) next = std::max(0, next);
        if (ImGui::IsItemDeactivatedAfterEdit()) m_WantCommit = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", targetLabel(next).c_str());
    };
    auto help = [](const char* text) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(380); ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
    };
    const char* condHelp = "A Lua expression, e.g.  game.gold >= 25  or  has_item(\"gem\")  or  not game.met_sage. Empty = always.";
    const char* actionHelp = "Lua statements, run when this happens, e.g.\n  game.gold = game.gold - 25\n  give(\"potion\")\n"
        "They see game, shared, scene, audio, events, level, `speaker` (the entity talking) and the helpers other "
        "scripts put in dialogue.helpers (inventory.lua: give, take, has_item).";
    const char* eventHelp = "A name sent to every script: on_event(self, name, data) with data = { dialogue, node, speaker }.";

    if (!n) {
        ImGui::TextUnformatted("Dialogue");
        ImGui::Separator();
        ImGui::TextDisabled("%s", m_Path.empty() ? "Not saved yet (File > Save As)" : ("Assets/" + m_Path).c_str());
        targetField("Start node", m_Start);
        ImGui::Spacing();
        std::vector<int> where;
        const auto issues = Issues(&where);
        if (!issues.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Problems");
            for (size_t i = 0; i < issues.size(); i++) {
                ImGui::PushID((int)i);
                if (ImGui::Selectable(issues[i].c_str()) && where[i]) m_Selected = where[i];
                ImGui::PopID();
            }
            ImGui::Spacing();
        }
        ImGui::TextDisabled("Select a node to edit it.");
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Line: someone speaks; with choices the player picks one, without it continues. "
            "Branch: goes one way or the other on a condition. Action: runs code or sends an event, then continues.");
        ImGui::TextDisabled("Text takes markup: {#ffcc66}colour{/}, {b}bold{/b}, and {=game.gold} shows a value.");
        ImGui::TextDisabled("In game: dialogue.open(\"%s\", { speaker = self }) (Scripts/Lib/dialogue.lua), or give an entity "
            "npc.lua and the matching name.", m_Path.empty() ? "name" : fs::path(m_Path).stem().string().c_str());
        ImGui::PopTextWrapPos();
        return;
    }

    ImGui::PushID(n->Id);
    ImGui::Text("#%d", n->Id);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    const char* types[] = { "line", "branch", "action" };
    int type = n->Type == "branch" ? 1 : n->Type == "action" ? 2 : 0;
    if (ImGui::Combo("##type", &type, "Line\0Branch\0Action\0")) { n->Type = types[type]; Commit(); }
    ImGui::SameLine();
    if (m_Start == n->Id) ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.5f, 1.0f), "Start");
    else if (ImGui::SmallButton("Start here")) { m_Start = n->Id; Commit(); }
    ImGui::Separator();

    if (n->Type == "line") {
        // speaker, with the names this dialogue already uses one click away
        ImGui::TextDisabled("Speaker (empty: the talking entity's name)");
        ImGui::SetNextItemWidth(-30);
        InputString("##speaker", n->Speaker);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##speakers", ImGuiDir_Down)) ImGui::OpenPopup("##speakerlist");
        if (ImGui::BeginPopup("##speakerlist")) {
            std::set<std::string> names;
            for (const Node& o : m_Nodes) if (!o.Speaker.empty()) names.insert(o.Speaker);
            if (names.empty()) ImGui::TextDisabled("Empty = the talking entity's name");
            for (const std::string& s : names) if (ImGui::MenuItem(s.c_str())) { n->Speaker = s; Commit(); }
            ImGui::EndPopup();
        }
        ImGui::TextDisabled("Text");
        InputString("##text", n->Text, true, 110);
        if (!n->Text.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", n->Text.c_str());
            ImGui::PopTextWrapPos();
        }
        if (ImGui::CollapsingHeader("When this line is shown", (!n->Action.empty() || !n->Event.empty()) ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            ImGui::TextDisabled("Action"); help(actionHelp);
            InputString("##action", n->Action, true, 50);
            ImGui::SetNextItemWidth(-80);
            InputString("Event", n->Event); help(eventHelp);
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Choices");
        if (n->Choices.empty()) {
            ImGui::TextDisabled("None: the line continues to");
            targetField("Next", n->Next);
        }
        int remove = -1, up = -1;
        for (size_t i = 0; i < n->Choices.size(); i++) {
            Choice& c = n->Choices[i];
            ImGui::PushID((int)i);
            ImGui::Separator();
            ImGui::Text("%zu.", i + 1);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-70);
            InputString("##ctext", c.Text);
            ImGui::SameLine();
            if (ImGui::SmallButton("^") && i > 0) up = (int)i;
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = (int)i;
            targetField("Goes to", c.Next);
            ImGui::SetNextItemWidth(-80);
            InputString("Condition", c.Cond); help(condHelp);
            if (!c.Cond.empty()) {
                bool disabled = c.Show == "disabled";
                if (ImGui::Checkbox("Show greyed out when false", &disabled)) { c.Show = disabled ? "disabled" : "hidden"; Commit(); }
                if (disabled) { ImGui::SetNextItemWidth(-80); InputString("Why", c.Why); }
            }
            if (ImGui::TreeNodeEx("When picked", (!c.Action.empty() || !c.Event.empty()) ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                ImGui::TextDisabled("Action"); help(actionHelp);
                InputString("##caction", c.Action, true, 46);
                ImGui::SetNextItemWidth(-80);
                InputString("Event", c.Event); help(eventHelp);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (remove >= 0) { n->Choices.erase(n->Choices.begin() + remove); Commit(); }
        if (up > 0) { std::swap(n->Choices[up], n->Choices[up - 1]); Commit(); }
        ImGui::Spacing();
        if (ImGui::Button("+ Add choice")) { n->Choices.push_back(Choice{ "...", "", "hidden", "", "", "", 0 }); Commit(); }
    }
    else if (n->Type == "branch") {
        ImGui::TextDisabled("Condition"); help(condHelp);
        InputString("##cond", n->Cond);
        targetField("If true", n->Next);
        targetField("If false", n->Else);
    }
    else {
        ImGui::TextDisabled("Action"); help(actionHelp);
        InputString("##action", n->Action, true, 90);
        ImGui::SetNextItemWidth(-80);
        InputString("Event", n->Event); help(eventHelp);
        targetField("Next", n->Next);
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Delete node")) { const int id = n->Id; ImGui::PopID(); Remove(id); Commit(); return; }
    ImGui::PopID();
}
