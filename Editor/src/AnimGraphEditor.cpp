#include "AnimGraphEditor.h"
#include "Scene/Components.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace Engine;

namespace {
    // Sizes in canvas units (pixels at zoom 1)
    constexpr float kNodeW = 200.0f, kHeaderH = 26.0f, kPinR = 6.0f, kPillW = 120.0f, kPillH = 30.0f, kBarH = 8.0f;

    ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
    ImVec2 Sub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
    ImVec2 Mul(ImVec2 a, float s) { return ImVec2(a.x * s, a.y * s); }
    float Len(ImVec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
    bool Inside(ImVec2 p, ImVec2 mn, ImVec2 mx) { return p.x >= mn.x && p.y >= mn.y && p.x <= mx.x && p.y <= mx.y; }
    ImVec2 V(glm::vec2 v) { return ImVec2(v.x, v.y); }
    glm::vec2 G(ImVec2 v) { return glm::vec2(v.x, v.y); }

    float SegmentDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
        const ImVec2 ab = Sub(b, a), ap = Sub(p, a);
        const float l2 = ab.x * ab.x + ab.y * ab.y;
        const float t = l2 > 0.0f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
        return Len(Sub(p, Add(a, Mul(ab, t))));
    }
    // Where the ray from a box's centre towards `toward` leaves the box
    ImVec2 EdgePoint(ImVec2 mn, ImVec2 mx, ImVec2 toward) {
        const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
        const ImVec2 d = Sub(toward, c);
        const float hx = (mx.x - mn.x) * 0.5f, hy = (mx.y - mn.y) * 0.5f;
        float t = 1e9f;
        if (std::abs(d.x) > 1e-4f) t = std::min(t, hx / std::abs(d.x));
        if (std::abs(d.y) > 1e-4f) t = std::min(t, hy / std::abs(d.y));
        if (t > 1e8f) return c;
        return Add(c, Mul(d, t));
    }

    int ResizeCallback(ImGuiInputTextCallbackData* d) {
        if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* s = static_cast<std::string*>(d->UserData);
            s->resize(d->BufTextLen);
            d->Buf = s->data();
        }
        return 0;
    }

    const char* TypeLabel(AnimParamType t) {
        return t == AnimParamType::Int ? "int" : t == AnimParamType::Bool ? "bool" : t == AnimParamType::Trigger ? "trigger" : "float";
    }
    std::string ConditionText(const AnimCondition& c, const AnimGraph& g) {
        const AnimParam* p = g.FindParam(c.Param);
        if (p && p->Type == AnimParamType::Trigger) return c.Param;
        char v[32];
        std::snprintf(v, sizeof(v), p && p->Type == AnimParamType::Int ? "%.0f" : "%g", c.Value);
        switch (c.Op) {
        case AnimCondOp::Greater:  return c.Param + " > " + v;
        case AnimCondOp::Less:     return c.Param + " < " + v;
        case AnimCondOp::Equal:    return c.Param + " == " + v;
        case AnimCondOp::NotEqual: return c.Param + " != " + v;
        case AnimCondOp::IsTrue:   return c.Param;
        case AnimCondOp::IsFalse:  return "not " + c.Param;
        }
        return c.Param;
    }
}

AnimGraphEditor::AnimGraphEditor(std::string assetDir) : m_AssetDir(std::move(assetDir)) {
    New();
}

// ------------------------------------------------------------------ file
bool AnimGraphEditor::Load(const std::string& path) {
    std::string error;
    auto g = AnimGraph::Load(m_AssetDir + path, &error);
    if (!g) { m_Status = "Can't open " + path + ": " + error; return false; }
    m_Graph = *g;
    m_Path = path;
    m_Layer = 0;
    m_Sel = Sel::None;
    m_SelState = 0;
    m_SelTransition = -1;
    m_SavedText = m_Graph.ToJson().dump();
    m_PublishedText.clear();
    ResetHistory();
    m_FrameNext = true;
    m_Status = "Opened " + path;
    Publish();
    return true;
}

bool AnimGraphEditor::SaveTo(const std::string& path) {
    if (!m_Graph.Save(m_AssetDir + path)) { m_Status = "Can't write " + path; return false; }
    m_Path = path;
    m_SavedText = m_Graph.ToJson().dump();
    m_PublishedText.clear();
    Publish();
    m_Status = "Saved " + path;
    return true;
}

bool AnimGraphEditor::SaveIfNamed() { return !m_Path.empty() && SaveTo(m_Path); }
bool AnimGraphEditor::IsDirty() const { return m_Graph.ToJson().dump() != m_SavedText; }

std::vector<std::string> AnimGraphEditor::ListFiles() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(m_AssetDir) / "AnimGraphs", ec))
        if (e.is_regular_file() && e.path().extension() == ".json") out.push_back("AnimGraphs/" + e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

void AnimGraphEditor::Publish() {
    if (m_Path.empty() || !OnChanged) return;
    std::string text = m_Graph.ToJson().dump();
    if (text == m_PublishedText) return;
    m_PublishedText = std::move(text);
    OnChanged(m_Path, std::make_shared<const AnimGraph>(m_Graph));
}

void AnimGraphEditor::New() {
    if (IsDirty() && !m_Graph.Layers.empty()) { m_PendingNew = true; m_AskUnsaved = true; return; }
    m_Graph = AnimGraph{};
    m_Graph.Layers.push_back(AnimGraphLayer{});
    m_Path.clear();
    m_Layer = 0;
    AnimState& s = AddState(AnimStateKind::Clip, ImVec2(240, 70), m_Clips.empty() ? std::string() : m_Clips.front());
    s.Name = "Idle";
    m_Sel = Sel::None;
    m_SavedText = m_Graph.ToJson().dump();
    m_PublishedText.clear();
    ResetHistory();
    m_FrameNext = true;
    m_Status.clear();
}

void AnimGraphEditor::Open(const std::string& path) {
    if (IsDirty()) { m_PendingOpen = path; m_PendingNew = false; m_AskUnsaved = true; return; }
    Load(path);
}

void AnimGraphEditor::SetLive(AnimatorComponent* animator, const std::string& entityName) {
    m_Live = animator;
    m_LiveName = entityName;
}

void AnimGraphEditor::SetModel(const std::vector<std::string>& clips, const std::vector<std::string>& bones) {
    m_Clips = clips;
    m_Bones = bones;
}

// ------------------------------------------------------------------ graph helpers
AnimGraphLayer& AnimGraphEditor::Layer() {
    if (m_Graph.Layers.empty()) m_Graph.Layers.push_back(AnimGraphLayer{});
    m_Layer = std::clamp(m_Layer, 0, (int)m_Graph.Layers.size() - 1);
    return m_Graph.Layers[m_Layer];
}

std::string AnimGraphEditor::UniqueStateName(const std::string& base) const {
    const AnimGraphLayer& L = m_Graph.Layers[m_Layer];
    auto used = [&](const std::string& n) { for (const AnimState& s : L.States) if (s.Name == n) return true; return false; };
    if (!used(base)) return base;
    for (int i = 2; ; i++) if (!used(base + " " + std::to_string(i))) return base + " " + std::to_string(i);
}

std::string AnimGraphEditor::UniqueParamName(const std::string& base) const {
    if (!m_Graph.FindParam(base)) return base;
    for (int i = 2; ; i++) if (!m_Graph.FindParam(base + std::to_string(i))) return base + std::to_string(i);
}

AnimState& AnimGraphEditor::AddState(AnimStateKind kind, ImVec2 pos, const std::string& clip) {
    AnimGraphLayer& L = Layer();
    AnimState s;
    s.Id = L.NewId();
    s.Kind = kind;
    s.Pos = G(pos);
    if (kind == AnimStateKind::Clip) {
        s.Clip = !clip.empty() ? clip : m_Clips.empty() ? std::string() : m_Clips.front();
        s.Name = UniqueStateName(s.Clip.empty() ? "State" : s.Clip);
    }
    else {
        s.Name = UniqueStateName("Blend");
        for (const AnimParam& p : m_Graph.Params)
            if (p.Type == AnimParamType::Float) { s.BlendParam = p.Name; break; }
        for (size_t i = 0; i < 2; i++)
            s.Points.push_back({ i < m_Clips.size() ? m_Clips[i] : std::string(), (float)i, 1.0f });
    }
    if (L.States.empty()) L.Entry = s.Id;
    L.States.push_back(s);
    m_Sel = Sel::State;
    m_SelState = s.Id;
    return L.States.back();
}

void AnimGraphEditor::RemoveState(int id) {
    AnimGraphLayer& L = Layer();
    L.States.erase(std::remove_if(L.States.begin(), L.States.end(), [&](const AnimState& s) { return s.Id == id; }), L.States.end());
    L.Transitions.erase(std::remove_if(L.Transitions.begin(), L.Transitions.end(),
        [&](const AnimTransition& t) { return t.From == id || t.To == id; }), L.Transitions.end());
    if (L.Entry == id) L.Entry = L.States.empty() ? 0 : L.States.front().Id;
    if (m_SelState == id) { m_Sel = Sel::None; m_SelState = 0; }
    m_SelTransition = -1;
}

void AnimGraphEditor::RemoveTransition(int index) {
    AnimGraphLayer& L = Layer();
    if (index < 0 || index >= (int)L.Transitions.size()) return;
    L.Transitions.erase(L.Transitions.begin() + index);
    if (m_Sel == Sel::Transition) m_Sel = Sel::None;
    m_SelTransition = -1;
}

// Rename a parameter everywhere it is used (and in the live animator's values)
void AnimGraphEditor::RenameParam(const std::string& from, const std::string& to) {
    if (from == to || from.empty()) return;
    auto fix = [&](std::string& s) { if (s == from) s = to; };
    for (AnimGraphLayer& L : m_Graph.Layers) {
        fix(L.WeightParam);
        for (AnimState& s : L.States) { fix(s.BlendParam); fix(s.SpeedParam); }
        for (AnimTransition& t : L.Transitions) for (AnimCondition& c : t.Conditions) fix(c.Param);
    }
    if (m_Live) {
        if (auto it = m_Live->Params.find(from); it != m_Live->Params.end()) { m_Live->Params[to] = it->second; m_Live->Params.erase(from); }
    }
}

// ------------------------------------------------------------------ undo
void AnimGraphEditor::ResetHistory() {
    m_History.assign(1, m_Graph.ToJson().dump());
    m_HistoryPos = 0;
}

void AnimGraphEditor::Commit() {
    const std::string s = m_Graph.ToJson().dump();
    if (m_HistoryPos >= 0 && m_History[m_HistoryPos] == s) return;
    m_History.resize(m_HistoryPos + 1);
    m_History.push_back(s);
    if (m_History.size() > 200) m_History.erase(m_History.begin());
    m_HistoryPos = (int)m_History.size() - 1;
}

void AnimGraphEditor::Undo() {
    Commit();
    if (m_HistoryPos <= 0) return;
    m_Graph = AnimGraph::FromJson(json::parse(m_History[--m_HistoryPos]));
    m_Sel = Sel::None;
    m_SelTransition = -1;
}

void AnimGraphEditor::Redo() {
    if (m_HistoryPos + 1 >= (int)m_History.size()) return;
    m_Graph = AnimGraph::FromJson(json::parse(m_History[++m_HistoryPos]));
    m_Sel = Sel::None;
    m_SelTransition = -1;
}

// ------------------------------------------------------------------ view helpers
ImVec2 AnimGraphEditor::ToScreen(ImVec2 c) const { return Add(Add(m_CanvasMin, m_Pan), Mul(c, m_Zoom)); }
ImVec2 AnimGraphEditor::ToCanvas(ImVec2 s) const { return Mul(Sub(Sub(s, m_CanvasMin), m_Pan), 1.0f / m_Zoom); }

AnimGraphEditor::Geo AnimGraphEditor::StateGeo(const AnimState& s) const {
    const float line = ImGui::GetFontSize() * 1.25f;
    const int lines = s.Kind == AnimStateKind::Clip ? 1 : 1 + (int)std::min<size_t>(s.Points.size(), 4);
    const float h = kHeaderH + lines * line + 10.0f + kBarH;
    Geo g;
    g.Min = ToScreen(V(s.Pos));
    g.Max = Add(g.Min, ImVec2(kNodeW * m_Zoom, h * m_Zoom));
    g.Out = ImVec2(g.Max.x, g.Min.y + kHeaderH * 0.5f * m_Zoom);
    return g;
}

AnimGraphEditor::Geo AnimGraphEditor::PillGeo(glm::vec2 pos) const {
    Geo g;
    g.Min = ToScreen(V(pos));
    g.Max = Add(g.Min, ImVec2(kPillW * m_Zoom, kPillH * m_Zoom));
    g.Out = ImVec2(g.Max.x, (g.Min.y + g.Max.y) * 0.5f);
    return g;
}

int AnimGraphEditor::StateAt(ImVec2 p) const {
    const AnimGraphLayer& L = m_Graph.Layers[m_Layer];
    if (m_Sel == Sel::State) if (const AnimState* s = L.Find(m_SelState)) { Geo g = StateGeo(*s); if (Inside(p, g.Min, g.Max)) return s->Id; }
    for (auto it = L.States.rbegin(); it != L.States.rend(); ++it) {
        Geo g = StateGeo(*it);
        if (Inside(p, g.Min, g.Max)) return it->Id;
    }
    return 0;
}

// The two ends of a transition's arrow on screen (box edges, shifted sideways so A->B and B->A sit apart).
// False for a state's transition to itself (drawn as a loop).
bool AnimGraphEditor::ArrowEnds(const AnimTransition& t, ImVec2& a, ImVec2& b) const {
    const AnimGraphLayer& L = m_Graph.Layers[m_Layer];
    const AnimState* to = L.Find(t.To);
    if (!to || t.From == t.To) return false;
    Geo from;
    if (t.From == AnimTransition::AnyState) from = PillGeo(L.AnyPos);
    else if (const AnimState* f = L.Find(t.From)) from = StateGeo(*f);
    else return false;
    const Geo dst = StateGeo(*to);
    const ImVec2 ca((from.Min.x + from.Max.x) * 0.5f, (from.Min.y + from.Max.y) * 0.5f);
    const ImVec2 cb((dst.Min.x + dst.Max.x) * 0.5f, (dst.Min.y + dst.Max.y) * 0.5f);
    ImVec2 d = Sub(cb, ca);
    const float len = Len(d);
    if (len < 1e-3f) return false;
    d = Mul(d, 1.0f / len);
    const ImVec2 side = Mul(ImVec2(-d.y, d.x), 6.0f * m_Zoom);
    a = Add(EdgePoint(from.Min, from.Max, Add(cb, side)), side);
    b = Add(EdgePoint(dst.Min, dst.Max, Add(ca, side)), side);
    return true;
}

int AnimGraphEditor::TransitionAt(ImVec2 p) const {
    const AnimGraphLayer& L = m_Graph.Layers[m_Layer];
    for (int i = (int)L.Transitions.size() - 1; i >= 0; i--) {
        const AnimTransition& t = L.Transitions[i];
        ImVec2 a, b;
        if (ArrowEnds(t, a, b)) { if (SegmentDistance(p, a, b) < 6.0f) return i; continue; }
        if (t.From == t.To) if (const AnimState* s = L.Find(t.To)) {        // the loop above the node
            const Geo g = StateGeo(*s);
            const ImVec2 c(g.Max.x - 24 * m_Zoom, g.Min.y);
            if (std::abs(Len(Sub(p, c)) - 12 * m_Zoom) < 5.0f) return i;
        }
    }
    return -1;
}

void AnimGraphEditor::FrameAll() {
    if (m_CanvasSize.x <= 0) return;
    const AnimGraphLayer& L = Layer();
    ImVec2 mn(FLT_MAX, FLT_MAX), mx(-FLT_MAX, -FLT_MAX);
    auto add = [&](glm::vec2 p, float w, float h) {
        mn = ImVec2(std::min(mn.x, p.x), std::min(mn.y, p.y));
        mx = ImVec2(std::max(mx.x, p.x + w), std::max(mx.y, p.y + h));
    };
    add(L.EntryPos, kPillW, kPillH);
    add(L.AnyPos, kPillW, kPillH);
    for (const AnimState& s : L.States) add(s.Pos, kNodeW, 110.0f);
    const float zx = (m_CanvasSize.x - 80) / std::max(1.0f, mx.x - mn.x), zy = (m_CanvasSize.y - 80) / std::max(1.0f, mx.y - mn.y);
    m_Zoom = std::clamp(std::min(zx, zy), 0.25f, 1.25f);
    m_Pan = Sub(ImVec2(40, 40), Mul(mn, m_Zoom));
}

bool AnimGraphEditor::InputString(const char* label, std::string& s) {
    const bool changed = ImGui::InputText(label, s.data(), s.capacity() + 1, ImGuiInputTextFlags_CallbackResize, ResizeCallback, &s);
    if (ImGui::IsItemDeactivatedAfterEdit()) m_WantCommit = true;
    return changed;
}

bool AnimGraphEditor::ClipCombo(const char* label, std::string& clip) {
    if (m_Clips.empty()) return InputString(label, clip);   // no model to list: type the name
    bool changed = false;
    const bool known = std::find(m_Clips.begin(), m_Clips.end(), clip) != m_Clips.end();
    const std::string preview = clip.empty() ? std::string("(none)") : known ? clip : clip + " (missing)";
    if (ImGui::BeginCombo(label, preview.c_str())) {
        for (const std::string& c : m_Clips)
            if (ImGui::Selectable(c.c_str(), c == clip)) { clip = c; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

bool AnimGraphEditor::ParamCombo(const char* label, std::string& param, bool numbersOnly, const char* none) {
    bool changed = false;
    const std::string preview = param.empty() ? std::string(none ? none : "(choose)") : m_Graph.FindParam(param) ? param : param + " (missing)";
    if (ImGui::BeginCombo(label, preview.c_str())) {
        if (none && ImGui::Selectable(none, param.empty())) { param.clear(); changed = true; }
        for (const AnimParam& p : m_Graph.Params) {
            if (numbersOnly && (p.Type == AnimParamType::Bool || p.Type == AnimParamType::Trigger)) continue;
            const std::string item = p.Name + "  (" + TypeLabel(p.Type) + ")";
            if (ImGui::Selectable(item.c_str(), p.Name == param)) { param = p.Name; changed = true; }
        }
        if (m_Graph.Params.empty()) ImGui::TextDisabled("No parameters yet: add them on the left");
        ImGui::EndCombo();
    }
    return changed;
}

// ------------------------------------------------------------------ the window
void AnimGraphEditor::Draw(bool* open) {
    const std::string name = m_Path.empty() ? std::string("(unsaved)") : fs::path(m_Path).filename().string();
    const std::string title = "AnimGraph Editor - " + name + (IsDirty() ? " *" : "") + "###AnimGraphEditor";
    ImGui::SetNextWindowSize(ImVec2(1280, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title.c_str(), open, ImGuiWindowFlags_MenuBar)) {
        m_Focused = false;
        ImGui::End();
        return;
    }
    m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    Layer();                                          // keeps m_Layer in range

    const ImGuiIO& io = ImGui::GetIO();
    if (m_Focused) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) { if (m_Path.empty()) m_AskSaveAs = true; else SaveTo(m_Path); }
        if (!io.WantTextInput) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) Redo(); else Undo(); }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && m_Sel == Sel::State) {
                if (const AnimState* s = Layer().Find(m_SelState)) {
                    AnimState copy = *s;
                    copy.Id = Layer().NewId();
                    copy.Name = UniqueStateName(copy.Name);
                    copy.Pos += glm::vec2(30.0f);
                    Layer().States.push_back(copy);
                    m_SelState = copy.Id;
                    Commit();
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                if (m_Sel == Sel::State) { RemoveState(m_SelState); Commit(); }
                else if (m_Sel == Sel::Transition) { RemoveTransition(m_SelTransition); Commit(); }
            }
            if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) FrameAll();
        }
    }

    DrawMenuBar();

    // The side panels take a share of the window (the canvas gets at least half of it)
    const float avail = ImGui::GetContentRegionAvail().x;
    const float sideW = std::clamp(avail * 0.24f, 250.0f, 320.0f), propsW = std::clamp(avail * 0.24f, 240.0f, 360.0f);
    ImGui::BeginChild("##side", ImVec2(sideW, 0), ImGuiChildFlags_Border);
    DrawSidePanel();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##canvas", ImVec2(ImGui::GetContentRegionAvail().x - propsW - 6.0f, 0), ImGuiChildFlags_Border,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    DrawCanvas();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##props", ImVec2(0, 0), ImGuiChildFlags_Border);
    DrawProperties();
    ImGui::EndChild();

    DrawPopups();
    // One undo step per finished interaction (nothing held), and every change goes live at once
    if (!ImGui::IsAnyItemActive() && m_Drag == Sel::None && m_LinkFrom == Sel::None) { Commit(); m_WantCommit = false; }
    Publish();
    ImGui::End();
}

void AnimGraphEditor::DrawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New")) New();
        if (ImGui::BeginMenu("Open")) {
            const auto files = ListFiles();
            if (files.empty()) ImGui::TextDisabled("No graphs in Assets/AnimGraphs yet");
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
        if (ImGui::MenuItem("Add State")) { AddState(AnimStateKind::Clip, at); Commit(); }
        if (ImGui::MenuItem("Add Blend State (1D)")) { AddState(AnimStateKind::Blend1D, at); Commit(); }
        if (ImGui::MenuItem("Delete", "Delete", false, m_Sel == Sel::State || m_Sel == Sel::Transition)) {
            if (m_Sel == Sel::State) RemoveState(m_SelState); else RemoveTransition(m_SelTransition);
            Commit();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Frame All", "F")) FrameAll();
        if (ImGui::MenuItem("Reset Zoom")) m_Zoom = 1.0f;
        ImGui::EndMenu();
    }
    const auto issues = m_Graph.Issues(m_Clips.empty() ? nullptr : &m_Clips);
    ImGui::Separator();
    if (issues.empty()) ImGui::TextDisabled("No problems");
    else ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%zu problem%s", issues.size(), issues.size() == 1 ? "" : "s");
    ImGui::Separator();
    if (m_Live) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "Live: %s", m_LiveName.c_str());
    else ImGui::TextDisabled(m_Path.empty() ? "Not live (save it, then give an Animator this graph)" : "Not live");
    if (!m_Status.empty()) { ImGui::Separator(); ImGui::TextDisabled("%s", m_Status.c_str()); }
    ImGui::EndMenuBar();
}

// ------------------------------------------------------------------ layers and parameters
void AnimGraphEditor::DrawSidePanel() {
    ImGui::SeparatorText("Layers");
    for (int i = 0; i < (int)m_Graph.Layers.size(); i++) {
        ImGui::PushID(i);
        const AnimGraphLayer& L = m_Graph.Layers[i];
        std::string label = L.Name + (i == 0 ? "  (base)" : L.Additive ? "  (additive)" : "");
        if (m_Live && i < (int)m_Live->GraphLayers.size()) label += "   > " + AnimGraphStateName(*m_Live, (size_t)i);
        if (ImGui::Selectable(label.c_str(), i == m_Layer)) {
            if (i != m_Layer) { m_Layer = i; m_Sel = Sel::None; m_SelTransition = -1; m_FrameNext = true; }
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ Layer")) {
        AnimGraphLayer L;
        L.Name = "Layer " + std::to_string(m_Graph.Layers.size() + 1);
        L.Weight = 1.0f;
        m_Graph.Layers.push_back(L);
        m_Layer = (int)m_Graph.Layers.size() - 1;
        m_Sel = Sel::None;
        AddState(AnimStateKind::Clip, ImVec2(240, 70));
        m_FrameNext = true;
        Commit();
    }
    AnimGraphLayer& L = Layer();
    if (m_Layer > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Up") && m_Layer > 1) { std::swap(m_Graph.Layers[m_Layer], m_Graph.Layers[m_Layer - 1]); m_Layer--; Commit(); return; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Down") && m_Layer + 1 < (int)m_Graph.Layers.size()) { std::swap(m_Graph.Layers[m_Layer], m_Graph.Layers[m_Layer + 1]); m_Layer++; Commit(); return; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete##layer")) {
            m_Graph.Layers.erase(m_Graph.Layers.begin() + m_Layer);
            m_Layer--;
            m_Sel = Sel::None;
            Commit();
            return;
        }
    }
    ImGui::PushItemWidth(-90.0f);
    InputString("Name##layer", L.Name);
    if (m_Layer == 0) ImGui::TextDisabled("The base layer: the whole skeleton");
    else {
        const std::string preview = L.Mask.empty() ? std::string("(whole skeleton)") : L.Mask;
        if (m_Bones.empty()) InputString("Mask", L.Mask);
        else if (ImGui::BeginCombo("Mask", preview.c_str())) {
            if (ImGui::Selectable("(whole skeleton)", L.Mask.empty())) L.Mask.clear();
            for (const std::string& b : m_Bones)
                if (ImGui::Selectable(b.c_str(), b == L.Mask)) L.Mask = b;
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("This bone and everything below it (a spine bone: the upper body)");
        ImGui::SliderFloat("Weight", &L.Weight, 0.0f, 1.0f);
        ParamCombo("Weight x", L.WeightParam, true, "(nothing)");
        ImGui::SetItemTooltip("Multiply the weight by a parameter (0..1), so scripts can fade the layer in and out");
        ImGui::Checkbox("Additive", &L.Additive);
        ImGui::SetItemTooltip("Add the states' motion (relative to their first frame) on top of the layers below,\n"
                              "instead of replacing the pose: a head turn or breathing over any walk");
    }
    ImGui::PopItemWidth();

    ImGui::SeparatorText("Parameters");
    std::string removeParam;
    if (ImGui::BeginTable("##params", m_Live ? 5 : 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        if (m_Live) ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 18.0f);
        ImGui::TableHeadersRow();
        static std::string renameFrom;
        for (size_t i = 0; i < m_Graph.Params.size(); i++) {
            AnimParam& p = m_Graph.Params[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            const std::string before = p.Name;
            InputString("##name", p.Name);
            if (ImGui::IsItemActivated()) renameFrom = before;
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (p.Name.empty() || std::count_if(m_Graph.Params.begin(), m_Graph.Params.end(), [&](const AnimParam& q) { return q.Name == p.Name; }) > 1)
                    p.Name = renameFrom;                  // empty or taken: keep the old name
                else RenameParam(renameFrom, p.Name);
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            int type = (int)p.Type;
            if (ImGui::Combo("##type", &type, "float\0int\0bool\0trigger\0")) { p.Type = (AnimParamType)type; if (p.Type == AnimParamType::Trigger) p.Default = 0.0f; }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            switch (p.Type) {
            case AnimParamType::Float: ImGui::DragFloat("##def", &p.Default, 0.01f); break;
            case AnimParamType::Int: { int v = (int)p.Default; if (ImGui::DragInt("##def", &v)) p.Default = (float)v; break; }
            case AnimParamType::Bool: { bool v = p.Default > 0.5f; if (ImGui::Checkbox("##def", &v)) p.Default = v ? 1.0f : 0.0f; break; }
            case AnimParamType::Trigger: ImGui::TextDisabled("-"); break;
            }
            if (m_Live) {
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                float& v = m_Live->Params[p.Name];        // the running game's value: change it to test the graph
                switch (p.Type) {
                case AnimParamType::Float: ImGui::DragFloat("##live", &v, 0.01f); break;
                case AnimParamType::Int: { int iv = (int)std::round(v); if (ImGui::DragInt("##live", &iv)) v = (float)iv; break; }
                case AnimParamType::Bool: { bool b = v > 0.5f; if (ImGui::Checkbox("##live", &b)) v = b ? 1.0f : 0.0f; break; }
                case AnimParamType::Trigger:
                    if (v > 0.5f) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "set");
                    else if (ImGui::SmallButton("Fire")) v = 1.0f;
                    break;
                }
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeParam = p.Name;
            ImGui::SetItemTooltip("Delete this parameter");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!removeParam.empty()) {
        m_Graph.Params.erase(std::remove_if(m_Graph.Params.begin(), m_Graph.Params.end(), [&](const AnimParam& p) { return p.Name == removeParam; }), m_Graph.Params.end());
        Commit();
    }
    const char* kinds[] = { "+ Float", "+ Int", "+ Bool", "+ Trigger" };
    const char* bases[] = { "value", "count", "flag", "trigger" };
    for (int k = 0; k < 4; k++) {
        if (k) ImGui::SameLine();
        if (ImGui::SmallButton(kinds[k])) {
            m_Graph.Params.push_back({ UniqueParamName(bases[k]), (AnimParamType)k, 0.0f });
            Commit();
        }
    }
    if (m_Live) ImGui::TextDisabled("Live: the values in the running game. Change them to try the graph.");
    else ImGui::TextDisabled("Scripts: e.animator:set(\"name\", v), :trigger(\"name\")");
}

// ------------------------------------------------------------------ canvas
void AnimGraphEditor::DrawCanvas() {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    AnimGraphLayer& L = Layer();
    m_CanvasMin = ImGui::GetCursorScreenPos();
    m_CanvasSize = ImGui::GetContentRegionAvail();
    if (m_CanvasSize.x < 50 || m_CanvasSize.y < 50) return;
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
        m_Zoom = std::clamp(m_Zoom * (io.MouseWheel > 0 ? 1.12f : 1.0f / 1.12f), 0.25f, 2.0f);
        const ImVec2 rel = Sub(Sub(mouse, m_CanvasMin), m_Pan);
        m_Pan = Sub(Sub(mouse, m_CanvasMin), Mul(rel, m_Zoom / old));
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
        m_Pan = Add(m_Pan, io.MouseDelta);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) m_RightDragged = true;
    }

    const Geo entry = PillGeo(L.EntryPos), any = PillGeo(L.AnyPos);
    auto nearPin = [&](ImVec2 pin) { return Len(Sub(mouse, pin)) <= (kPinR + 4.0f) * m_Zoom; };

    // ---- left button: a transition from a pin, move a node, select an arrow, or select nothing
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_LinkFrom = Sel::None;
        if (nearPin(entry.Out)) m_LinkFrom = Sel::Entry;
        else if (nearPin(any.Out)) m_LinkFrom = Sel::Any;
        else for (const AnimState& s : L.States) if (nearPin(StateGeo(s).Out)) { m_LinkFrom = Sel::State; m_LinkState = s.Id; break; }
        if (m_LinkFrom == Sel::None) {
            if (const int id = StateAt(mouse)) { m_Sel = Sel::State; m_SelState = id; m_Drag = Sel::State; m_DragState = id; m_DragMoved = false; }
            else if (Inside(mouse, entry.Min, entry.Max)) { m_Sel = Sel::Entry; m_Drag = Sel::Entry; m_DragMoved = false; }
            else if (Inside(mouse, any.Min, any.Max)) { m_Sel = Sel::Any; m_Drag = Sel::Any; m_DragMoved = false; }
            else if (const int t = TransitionAt(mouse); t >= 0) { m_Sel = Sel::Transition; m_SelTransition = t; }
            else m_Sel = Sel::None;
        }
    }
    if (m_Drag != Sel::None && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        const glm::vec2 d = G(Mul(io.MouseDelta, 1.0f / m_Zoom));
        if (m_Drag == Sel::State) { if (AnimState* s = L.Find(m_DragState)) s->Pos += d; }
        else if (m_Drag == Sel::Entry) L.EntryPos += d;
        else if (m_Drag == Sel::Any) L.AnyPos += d;
        m_DragMoved = true;
    }
    if (m_Drag != Sel::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) m_Drag = Sel::None;   // committed by Draw
    if (m_LinkFrom != Sel::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (const int target = StateAt(mouse)) {
            if (m_LinkFrom == Sel::Entry) L.Entry = target;
            else {
                AnimTransition t;
                t.From = m_LinkFrom == Sel::Any ? AnimTransition::AnyState : m_LinkState;
                t.To = target;
                L.Transitions.push_back(t);
                m_Sel = Sel::Transition;
                m_SelTransition = (int)L.Transitions.size() - 1;
            }
        }
        m_LinkFrom = Sel::None;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !StateAt(mouse) && TransitionAt(mouse) < 0 &&
        !Inside(mouse, entry.Min, entry.Max) && !Inside(mouse, any.Min, any.Max)) {
        AddState(AnimStateKind::Clip, ToCanvas(mouse));
    }

    // ---- right click (not a drag): state menu, transition menu, or the add menu
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !m_RightDragged) {
        if (const int id = StateAt(mouse)) { m_Sel = Sel::State; m_SelState = id; m_ContextState = id; ImGui::OpenPopup("##statemenu"); }
        else if (const int t = TransitionAt(mouse); t >= 0) { m_Sel = Sel::Transition; m_SelTransition = t; m_ContextTransition = t; ImGui::OpenPopup("##transitionmenu"); }
        else { m_PopupPos = ToCanvas(mouse); ImGui::OpenPopup("##addmenu"); }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) m_RightDragged = false;

    // ---- live state of this layer
    const AnimGraphLayerState* live = (m_Live && m_Layer < (int)m_Live->GraphLayers.size()) ? &m_Live->GraphLayers[m_Layer] : nullptr;
    const bool liveFading = live && live->Fade < 1.0f;

    // ---- draw
    dl->PushClipRect(m_CanvasMin, canvasMax, true);
    dl->AddRectFilled(m_CanvasMin, canvasMax, IM_COL32(24, 26, 31, 255));
    const float grid = 32.0f * m_Zoom;
    for (float x = std::fmod(m_Pan.x, grid); x < m_CanvasSize.x; x += grid)
        dl->AddLine(ImVec2(m_CanvasMin.x + x, m_CanvasMin.y), ImVec2(m_CanvasMin.x + x, canvasMax.y), IM_COL32(255, 255, 255, 10));
    for (float y = std::fmod(m_Pan.y, grid); y < m_CanvasSize.y; y += grid)
        dl->AddLine(ImVec2(m_CanvasMin.x, m_CanvasMin.y + y), ImVec2(canvasMax.x, m_CanvasMin.y + y), IM_COL32(255, 255, 255, 10));

    auto arrow = [&](ImVec2 a, ImVec2 b, ImU32 col, float thick) {
        dl->AddLine(a, b, col, thick * m_Zoom);
        const ImVec2 d = Sub(b, a);
        const float len = Len(d);
        if (len < 1.0f) return;
        const ImVec2 u = Mul(d, 1.0f / len), n(-u.y, u.x);
        const ImVec2 tip = Add(a, Mul(d, 0.5f));
        const float s = 7.0f * m_Zoom;
        const ImVec2 back = Sub(tip, Mul(u, s * 1.6f));
        dl->AddTriangleFilled(Add(tip, Mul(u, s * 0.4f)), Add(back, Mul(n, s)), Sub(back, Mul(n, s)), col);
    };

    // Entry link
    if (const AnimState* e = L.Find(L.Entry)) {
        const Geo g = StateGeo(*e);
        arrow(EdgePoint(entry.Min, entry.Max, ImVec2((g.Min.x + g.Max.x) * 0.5f, (g.Min.y + g.Max.y) * 0.5f)),
              EdgePoint(g.Min, g.Max, ImVec2((entry.Min.x + entry.Max.x) * 0.5f, (entry.Min.y + entry.Max.y) * 0.5f)),
              IM_COL32(240, 160, 60, 220), 2.0f);
    }
    // Transitions
    for (int i = 0; i < (int)L.Transitions.size(); i++) {
        const AnimTransition& t = L.Transitions[i];
        const bool selected = m_Sel == Sel::Transition && m_SelTransition == i;
        const bool active = liveFading && live->Transition == i;
        ImU32 col = t.From == AnimTransition::AnyState ? IM_COL32(110, 200, 210, 200) : IM_COL32(185, 192, 210, 200);
        if (t.Conditions.empty() && !t.HasExitTime) col = IM_COL32(230, 110, 100, 200);   // never taken
        if (active) col = IM_COL32(120, 235, 140, 255);
        if (selected) col = IM_COL32(255, 205, 90, 255);
        ImVec2 a, b;
        if (ArrowEnds(t, a, b)) arrow(a, b, col, selected || active ? 3.0f : 2.0f);
        else if (t.From == t.To) if (const AnimState* s = L.Find(t.To)) {
            const Geo g = StateGeo(*s);
            const ImVec2 c(g.Max.x - 24 * m_Zoom, g.Min.y);
            dl->AddCircle(c, 12 * m_Zoom, col, 0, (selected ? 3.0f : 2.0f) * m_Zoom);
        }
    }

    // Nodes
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * m_Zoom;
    auto pin = [&](ImVec2 p) {
        dl->AddCircleFilled(p, kPinR * m_Zoom, IM_COL32(60, 64, 76, 255));
        dl->AddCircle(p, kPinR * m_Zoom, IM_COL32(220, 225, 235, 255), 0, 1.5f);
    };
    auto pill = [&](const Geo& g, const char* text, ImU32 fill, bool selected) {
        const float r = kPillH * 0.5f * m_Zoom;
        dl->AddRectFilled(Add(g.Min, ImVec2(3, 4)), Add(g.Max, ImVec2(3, 4)), IM_COL32(0, 0, 0, 70), r);
        dl->AddRectFilled(g.Min, g.Max, fill, r);
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, text);
        dl->AddText(font, fs, ImVec2((g.Min.x + g.Max.x - ts.x) * 0.5f, (g.Min.y + g.Max.y - ts.y) * 0.5f), IM_COL32(255, 255, 255, 240), text);
        if (selected) dl->AddRect(g.Min, g.Max, IM_COL32(255, 200, 80, 255), r, 0, 2.5f);
        pin(g.Out);
    };
    pill(entry, "Entry", IM_COL32(52, 128, 72, 255), m_Sel == Sel::Entry);
    pill(any, "Any State", IM_COL32(36, 112, 122, 255), m_Sel == Sel::Any);

    const float line = ImGui::GetFontSize() * 1.25f * m_Zoom;
    auto drawState = [&](const AnimState& s) {
        const Geo g = StateGeo(s);
        const float r = 6.0f * m_Zoom;
        const bool blend = s.Kind == AnimStateKind::Blend1D;
        dl->AddRectFilled(Add(g.Min, ImVec2(3, 4)), Add(g.Max, ImVec2(3, 4)), IM_COL32(0, 0, 0, 70), r);
        dl->AddRectFilled(g.Min, g.Max, IM_COL32(36, 39, 47, 245), r);
        const float headerBottom = g.Min.y + kHeaderH * m_Zoom;
        dl->AddRectFilled(g.Min, ImVec2(g.Max.x, headerBottom), blend ? IM_COL32(118, 72, 150, 255) : IM_COL32(52, 88, 150, 255), r, ImDrawFlags_RoundCornersTop);
        dl->PushClipRect(g.Min, g.Max, true);
        dl->AddText(font, fs, ImVec2(g.Min.x + 8 * m_Zoom, g.Min.y + (kHeaderH * m_Zoom - fs) * 0.5f), IM_COL32(255, 255, 255, 240), s.Name.c_str());
        if (s.Id == L.Entry) {
            const char* tag = "ENTRY";
            const ImVec2 ts = font->CalcTextSizeA(fs * 0.75f, FLT_MAX, 0, tag);
            const ImVec2 p0(g.Max.x - ts.x - 18 * m_Zoom, g.Min.y + 6 * m_Zoom);
            dl->AddRectFilled(p0, Add(p0, ImVec2(ts.x + 8 * m_Zoom, kHeaderH * m_Zoom - 12 * m_Zoom)), IM_COL32(200, 130, 40, 255), 4 * m_Zoom);
            dl->AddText(font, fs * 0.75f, Add(p0, ImVec2(4 * m_Zoom, 1 * m_Zoom)), IM_COL32(255, 255, 255, 255), tag);
        }
        // body: the clip, or the blend parameter and its clips
        float y = headerBottom + 5 * m_Zoom;
        auto text = [&](const std::string& t, ImU32 col) {
            dl->AddText(font, fs * 0.95f, ImVec2(g.Min.x + 8 * m_Zoom, y), col, t.c_str());
            y += line;
        };
        auto clipCol = [&](const std::string& c) {
            const bool ok = !c.empty() && (m_Clips.empty() || std::find(m_Clips.begin(), m_Clips.end(), c) != m_Clips.end());
            return ok ? IM_COL32(220, 222, 228, 230) : IM_COL32(240, 120, 100, 240);
        };
        if (!blend) text(s.Clip.empty() ? std::string("(no clip)") : "> " + s.Clip + (s.Loop ? "" : "  (once)"), clipCol(s.Clip));
        else {
            text("blend by " + (s.BlendParam.empty() ? std::string("?") : s.BlendParam), IM_COL32(210, 190, 240, 230));
            for (size_t i = 0; i < s.Points.size() && i < 4; i++) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "  %s @ %g", s.Points[i].Clip.empty() ? "?" : s.Points[i].Clip.c_str(), s.Points[i].Threshold);
                text(buf, clipCol(s.Points[i].Clip));
            }
        }
        // live progress bar along the bottom
        const bool current = live && live->State == s.Id, fadingOut = liveFading && live->From == s.Id;
        const ImVec2 b0(g.Min.x + 6 * m_Zoom, g.Max.y - (kBarH + 2) * m_Zoom), b1(g.Max.x - 6 * m_Zoom, g.Max.y - 4 * m_Zoom);
        if (current || fadingOut) {
            const float phase = current ? live->Phase : live->FromPhase;
            dl->AddRectFilled(b0, b1, IM_COL32(20, 22, 26, 255), 2 * m_Zoom);
            dl->AddRectFilled(b0, ImVec2(b0.x + (b1.x - b0.x) * std::clamp(phase, 0.0f, 1.0f), b1.y),
                              current ? IM_COL32(110, 220, 130, 255) : IM_COL32(230, 160, 80, 200), 2 * m_Zoom);
        }
        dl->PopClipRect();
        pin(g.Out);
        const bool selected = m_Sel == Sel::State && m_SelState == s.Id;
        ImU32 border = IM_COL32(255, 255, 255, 30);
        float thick = 1.5f;
        if (fadingOut) { border = IM_COL32(230, 160, 80, 220); thick = 2.0f; }
        if (current) { border = IM_COL32(110, 230, 130, 255); thick = 3.0f; }
        if (selected) { border = IM_COL32(255, 200, 80, 255); thick = 2.5f; }
        dl->AddRect(g.Min, g.Max, border, r, 0, thick);
        if (current && selected) dl->AddRect(Sub(g.Min, ImVec2(3, 3)), Add(g.Max, ImVec2(3, 3)), IM_COL32(110, 230, 130, 200), r + 3, 0, 2.0f);
    };
    for (const AnimState& s : L.States) if (!(m_Sel == Sel::State && s.Id == m_SelState)) drawState(s);
    if (m_Sel == Sel::State) if (const AnimState* s = L.Find(m_SelState)) drawState(*s);

    // the transition being dragged out
    if (m_LinkFrom != Sel::None) {
        ImVec2 from = m_LinkFrom == Sel::Entry ? entry.Out : any.Out;
        if (m_LinkFrom == Sel::State) if (const AnimState* s = L.Find(m_LinkState)) from = StateGeo(*s).Out;
        arrow(from, mouse, IM_COL32(255, 210, 100, 255), 2.5f);
    }

    dl->AddText(ImVec2(m_CanvasMin.x + 8, m_CanvasMin.y + 6), IM_COL32(255, 255, 255, 110), (L.Name + (m_Layer == 0 ? "  (base layer)" : "")).c_str());
    dl->AddText(ImVec2(m_CanvasMin.x + 8, canvasMax.y - 22), IM_COL32(255, 255, 255, 80),
        "Drag a pin onto a state: transition (from Entry: the entry state).  Click an arrow to select it.  Right-click: menus.  Double-click: new state.  F: frame");
    dl->PopClipRect();

    // ---- popups (opened above, drawn in this child)
    if (ImGui::BeginPopup("##addmenu")) {
        if (ImGui::MenuItem("State")) { AddState(AnimStateKind::Clip, m_PopupPos); Commit(); }
        if (ImGui::MenuItem("Blend State (1D)")) { AddState(AnimStateKind::Blend1D, m_PopupPos); Commit(); }
        if (!m_Clips.empty() && ImGui::BeginMenu("State for Clip")) {
            for (const std::string& c : m_Clips)
                if (ImGui::MenuItem(c.c_str())) { AddState(AnimStateKind::Clip, m_PopupPos, c); Commit(); }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##statemenu")) {
        if (AnimState* s = L.Find(m_ContextState)) {
            ImGui::TextDisabled("%s", s->Name.c_str());
            if (ImGui::MenuItem("Set as Entry", nullptr, L.Entry == s->Id)) { L.Entry = s->Id; Commit(); }
            if (ImGui::MenuItem("Transition to Itself")) {
                AnimTransition t; t.From = t.To = s->Id; t.HasExitTime = true; t.ExitTime = 1.0f;
                L.Transitions.push_back(t);
                m_Sel = Sel::Transition; m_SelTransition = (int)L.Transitions.size() - 1;
                Commit();
            }
            if (m_Live && ImGui::MenuItem("Play Now (live)")) AnimGraphPlayState(*m_Live, (size_t)m_Layer, s->Name, 0.2f);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                AnimState copy = *s;
                copy.Id = L.NewId();
                copy.Name = UniqueStateName(copy.Name);
                copy.Pos += glm::vec2(30.0f);
                L.States.push_back(copy);
                m_Sel = Sel::State; m_SelState = copy.Id;
                Commit();
            }
            if (ImGui::MenuItem("Delete", "Delete")) { RemoveState(m_ContextState); Commit(); }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##transitionmenu")) {
        if (m_ContextTransition >= 0 && m_ContextTransition < (int)L.Transitions.size()) {
            if (ImGui::MenuItem("Delete", "Delete")) { RemoveTransition(m_ContextTransition); Commit(); }
        }
        ImGui::EndPopup();
    }
}

// ------------------------------------------------------------------ properties
void AnimGraphEditor::DrawProperties() {
    AnimGraphLayer& L = Layer();
    if (m_Sel == Sel::State) {
        if (AnimState* s = L.Find(m_SelState)) DrawStateProperties(*s);
        else m_Sel = Sel::None;
    }
    else if (m_Sel == Sel::Transition) {
        if (m_SelTransition >= 0 && m_SelTransition < (int)L.Transitions.size()) DrawTransitionProperties(m_SelTransition);
        else m_Sel = Sel::None;
    }
    else if (m_Sel == Sel::Entry) {
        ImGui::SeparatorText("Entry");
        const AnimState* e = L.Find(L.Entry);
        ImGui::TextWrapped("The layer starts in %s. Drag Entry's pin onto a state to change it.", e ? e->Name.c_str() : "(nothing)");
    }
    else if (m_Sel == Sel::Any) {
        ImGui::SeparatorText("Any State");
        ImGui::TextWrapped("Transitions from Any State are checked whatever the current state is (before the state's own), "
                           "and even during a cross-fade: hits, deaths, jumps. Drag its pin onto a state to add one; "
                           "give it a condition (a trigger, usually).");
    }
    else {
        ImGui::SeparatorText("Graph");
        ImGui::TextWrapped("Select a state or an arrow to edit it.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("States play a clip, or blend clips along a parameter (walk at 1, run at 4). "
                           "Arrows are transitions: taken when their conditions hold (and the exit time has passed), "
                           "cross-fading for their duration. Layers play their own state machines over a part of the "
                           "skeleton. Give an Animator this graph in the Inspector.");
        ImGui::PopStyleColor();
    }

    // live summary
    if (m_Live && m_Layer < (int)m_Live->GraphLayers.size()) {
        const AnimGraphLayerState& rt = m_Live->GraphLayers[m_Layer];
        ImGui::SeparatorText("Live");
        ImGui::Text("%s: %s", m_LiveName.c_str(), AnimGraphStateName(*m_Live, (size_t)m_Layer).c_str());
        ImGui::ProgressBar(rt.Phase, ImVec2(-FLT_MIN, 0), nullptr);
        ImGui::TextDisabled("%.2f s in the state%s", rt.StateTime, rt.Fade < 1.0f ? ", cross-fading" : "");
    }

    const auto issues = m_Graph.Issues(m_Clips.empty() ? nullptr : &m_Clips);
    ImGui::SeparatorText(issues.empty() ? "No problems" : "Problems");
    for (const std::string& i : issues) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", i.c_str());
    if (m_Clips.empty()) ImGui::TextDisabled("Select an entity with an animated model to check clip names.");
}

void AnimGraphEditor::DrawStateProperties(AnimState& s) {
    AnimGraphLayer& L = Layer();
    ImGui::SeparatorText(s.Kind == AnimStateKind::Blend1D ? "Blend state" : "State");
    ImGui::PushItemWidth(-110.0f);
    InputString("Name", s.Name);
    int kind = (int)s.Kind;
    ImGui::RadioButton("Clip", &kind, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Blend 1D", &kind, 1);
    if (kind != (int)s.Kind) {
        s.Kind = (AnimStateKind)kind;
        if (s.Kind == AnimStateKind::Blend1D && s.Points.empty()) {
            s.Points.push_back({ s.Clip, 0.0f, 1.0f });
            s.Points.push_back({ m_Clips.size() > 1 ? m_Clips[1] : std::string(), 1.0f, 1.0f });
        }
    }
    if (s.Kind == AnimStateKind::Clip) ClipCombo("Clip", s.Clip);
    else {
        ParamCombo("Blend by", s.BlendParam, true);
        ImGui::SetItemTooltip("A number parameter: each clip below plays alone at its threshold, the two around\n"
                              "the value blend (and stay in step), beyond either end the end clip plays");
        if (m_Live && !s.BlendParam.empty())
            if (auto it = m_Live->Params.find(s.BlendParam); it != m_Live->Params.end())
                ImGui::TextDisabled("Live value: %.2f", it->second);
        int remove = -1;
        if (ImGui::BeginTable("##points", 4, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("At", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 18.0f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < s.Points.size(); i++) {
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-FLT_MIN); ClipCombo("##clip", s.Points[i].Clip);
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##at", &s.Points[i].Threshold, 0.01f);
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-FLT_MIN); ImGui::DragFloat("##speed", &s.Points[i].Speed, 0.01f, 0.01f, 10.0f);
                ImGui::TableNextColumn(); if (ImGui::SmallButton("x")) remove = (int)i;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (remove >= 0) s.Points.erase(s.Points.begin() + remove);
        if (ImGui::SmallButton("+ Clip")) {
            const float next = s.Points.empty() ? 0.0f : s.Points.back().Threshold + 1.0f;
            s.Points.push_back({ m_Clips.empty() ? std::string() : m_Clips.front(), next, 1.0f });
        }
    }
    ImGui::DragFloat("Speed", &s.Speed, 0.01f, -5.0f, 10.0f);
    ParamCombo("Speed x", s.SpeedParam, true, "(nothing)");
    ImGui::SetItemTooltip("Multiply the speed by a parameter (a walk that speeds up with the character)");
    ImGui::Checkbox("Loop", &s.Loop);
    ImGui::SetItemTooltip("Off: play once and hold the last frame (fires \"end\"); use an exit time to move on");
    ImGui::PopItemWidth();
    if (L.Entry != s.Id) { if (ImGui::Button("Set as Entry")) L.Entry = s.Id; ImGui::SameLine(); }
    if (m_Live) { if (ImGui::Button("Play now")) AnimGraphPlayState(*m_Live, (size_t)m_Layer, s.Name, 0.2f); ImGui::SameLine(); }
    if (ImGui::Button("Delete state")) { RemoveState(s.Id); Commit(); return; }

    ImGui::SeparatorText("Transitions out");
    bool any = false;
    for (int i = 0; i < (int)L.Transitions.size(); i++) {
        const AnimTransition& t = L.Transitions[i];
        if (t.From != s.Id) continue;
        any = true;
        const AnimState* to = L.Find(t.To);
        std::string label = "-> " + (to ? to->Name : std::string("?"));
        for (size_t c = 0; c < t.Conditions.size(); c++) label += (c ? " and " : "  if ") + ConditionText(t.Conditions[c], m_Graph);
        if (t.HasExitTime) { char b[32]; std::snprintf(b, sizeof(b), "  (after %.0f%%)", t.ExitTime * 100.0f); label += b; }
        ImGui::PushID(i);
        if (ImGui::Selectable(label.c_str())) { m_Sel = Sel::Transition; m_SelTransition = i; }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("None: drag this state's pin onto another");
}

void AnimGraphEditor::DrawTransitionProperties(int index) {
    AnimGraphLayer& L = Layer();
    AnimTransition& t = L.Transitions[index];
    const AnimState* from = L.Find(t.From);
    const AnimState* to = L.Find(t.To);
    ImGui::SeparatorText("Transition");
    ImGui::Text("%s  ->  %s", t.From == AnimTransition::AnyState ? "Any State" : from ? from->Name.c_str() : "?", to ? to->Name.c_str() : "?");
    ImGui::PushItemWidth(-110.0f);
    if (ImGui::BeginCombo("To", to ? to->Name.c_str() : "?")) {
        for (const AnimState& s : L.States)
            if (ImGui::Selectable(s.Name.c_str(), s.Id == t.To)) t.To = s.Id;
        ImGui::EndCombo();
    }
    ImGui::DragFloat("Blend", &t.Duration, 0.005f, 0.0f, 5.0f, "%.2f s");
    ImGui::SetItemTooltip("Cross-fade time");
    ImGui::Checkbox("Exit time", &t.HasExitTime);
    ImGui::SetItemTooltip("Wait until the state has played this far (a share of its length) before moving on.\n"
                          "Alone (no conditions): move on when it gets there, like the end of an attack.");
    if (t.HasExitTime) ImGui::SliderFloat("At", &t.ExitTime, 0.0f, 1.0f, "%.2f of the state");

    ImGui::SeparatorText("Conditions (all must hold)");
    int remove = -1;
    for (size_t i = 0; i < t.Conditions.size(); i++) {
        AnimCondition& c = t.Conditions[i];
        ImGui::PushID((int)i);
        const AnimParam* p = m_Graph.FindParam(c.Param);
        // live: does it hold right now?
        if (m_Live) {
            bool holds = false;
            if (p) {
                auto it = m_Live->Params.find(c.Param);
                const float v = it != m_Live->Params.end() ? it->second : p->Default;
                switch (c.Op) {
                case AnimCondOp::Greater: holds = v > c.Value; break;
                case AnimCondOp::Less: holds = v < c.Value; break;
                case AnimCondOp::Equal: holds = std::abs(v - c.Value) < 1e-4f; break;
                case AnimCondOp::NotEqual: holds = std::abs(v - c.Value) >= 1e-4f; break;
                case AnimCondOp::IsTrue: holds = v > 0.5f; break;
                case AnimCondOp::IsFalse: holds = v <= 0.5f; break;
                }
            }
            ImGui::TextColored(holds ? ImVec4(0.45f, 0.9f, 0.5f, 1.0f) : ImVec4(0.9f, 0.4f, 0.35f, 1.0f), holds ? "yes" : "no ");
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.42f);
        if (ParamCombo("##param", c.Param, false)) {
            p = m_Graph.FindParam(c.Param);
            if (p && (p->Type == AnimParamType::Bool || p->Type == AnimParamType::Trigger)) c.Op = AnimCondOp::IsTrue;
            else if (c.Op == AnimCondOp::IsTrue || c.Op == AnimCondOp::IsFalse) c.Op = AnimCondOp::Greater;
        }
        ImGui::SameLine();
        if (p && p->Type == AnimParamType::Trigger) ImGui::TextDisabled("is fired");
        else if (p && p->Type == AnimParamType::Bool) {
            ImGui::SetNextItemWidth(-28.0f);
            int op = c.Op == AnimCondOp::IsFalse ? 1 : 0;
            if (ImGui::Combo("##op", &op, "is true\0is false\0")) c.Op = op ? AnimCondOp::IsFalse : AnimCondOp::IsTrue;
        }
        else {
            ImGui::SetNextItemWidth(48.0f);
            int op = std::min((int)c.Op, 3);
            if (ImGui::Combo("##op", &op, ">\0<\0==\0!=\0")) c.Op = (AnimCondOp)op;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-28.0f);
            if (p && p->Type == AnimParamType::Int) { int v = (int)c.Value; if (ImGui::DragInt("##v", &v)) c.Value = (float)v; }
            else ImGui::DragFloat("##v", &c.Value, 0.01f);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = (int)i;
        ImGui::PopID();
    }
    if (remove >= 0) t.Conditions.erase(t.Conditions.begin() + remove);
    if (ImGui::SmallButton("+ Condition")) {
        AnimCondition c;
        if (!m_Graph.Params.empty()) {
            c.Param = m_Graph.Params.front().Name;
            const AnimParamType type = m_Graph.Params.front().Type;
            if (type == AnimParamType::Bool || type == AnimParamType::Trigger) c.Op = AnimCondOp::IsTrue;
        }
        t.Conditions.push_back(c);
    }
    if (t.Conditions.empty() && !t.HasExitTime) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "No conditions and no exit time: never taken");
    ImGui::PopItemWidth();

    ImGui::SeparatorText("Order");
    ImGui::TextDisabled("When several could be taken, the first in the list wins");
    if (ImGui::SmallButton("Earlier") && index > 0) { std::swap(L.Transitions[index], L.Transitions[index - 1]); m_SelTransition = index - 1; Commit(); return; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Later") && index + 1 < (int)L.Transitions.size()) { std::swap(L.Transitions[index], L.Transitions[index + 1]); m_SelTransition = index + 1; Commit(); return; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete transition")) { RemoveTransition(index); Commit(); }
}

// ------------------------------------------------------------------ dialogs
void AnimGraphEditor::DrawPopups() {
    if (m_AskUnsaved) { ImGui::OpenPopup("Unsaved animation graph"); m_AskUnsaved = false; }
    if (ImGui::BeginPopupModal("Unsaved animation graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has unsaved changes.", m_Path.empty() ? "This graph" : m_Path.c_str());
        auto proceed = [&](bool discarded) {
            // Animators using it were playing the unsaved edits: back to the file
            if (discarded && !m_Path.empty() && OnChanged)
                if (auto g = AnimGraph::Load(m_AssetDir + m_Path)) OnChanged(m_Path, g);
            m_SavedText = m_Graph.ToJson().dump();
            if (m_PendingNew) New(); else Load(m_PendingOpen);
            ImGui::CloseCurrentPopup();
        };
        if (!m_Path.empty() && ImGui::Button("Save", ImVec2(100, 0))) { if (SaveTo(m_Path)) proceed(false); }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100, 0))) proceed(true);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_AskSaveAs) {
        ImGui::OpenPopup("Save animation graph as");
        m_AskSaveAs = false;
        const std::string stem = m_Path.empty() ? std::string("new_graph") : fs::path(m_Path).stem().string();
        std::snprintf(m_SaveAsName, sizeof(m_SaveAsName), "%s", stem.c_str());
    }
    if (ImGui::BeginPopupModal("Save animation graph as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Assets/AnimGraphs/");
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText(".json", m_SaveAsName, sizeof(m_SaveAsName), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::TextDisabled("Then pick it as the Graph of an Animator (Inspector)");
        if ((ImGui::Button("Save", ImVec2(100, 0)) || enter) && m_SaveAsName[0]) {
            SaveTo(std::string("AnimGraphs/") + m_SaveAsName + ".json");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
