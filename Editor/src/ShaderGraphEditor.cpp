#include "ShaderGraphEditor.h"
#include "AssetBrowser.h"

#include "Renderer/Shader.h"
#include "Runtime/ResourceLibrary.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace Engine;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
    constexpr float kHeader = 24.0f, kRow = 20.0f, kPad = 8.0f, kPinR = 5.0f;
    constexpr const char* kParamPayload = "MDB_SG_PARAM";

    ImU32 CategoryColor(const std::string& c) {
        if (c == "Output") return IM_COL32(150, 150, 160, 255);
        if (c == "Input") return IM_COL32(70, 140, 80, 255);
        if (c == "Texture") return IM_COL32(140, 80, 170, 255);
        if (c == "UV") return IM_COL32(50, 140, 140, 255);
        if (c == "Math") return IM_COL32(60, 100, 170, 255);
        if (c == "Vector") return IM_COL32(60, 150, 190, 255);
        if (c == "Procedural") return IM_COL32(190, 120, 50, 255);
        if (c == "Surface") return IM_COL32(170, 150, 50, 255);
        return IM_COL32(170, 70, 70, 255);
    }
    ImU32 TypeColor(int t) {
        switch (t) {
        case 1: return IM_COL32(150, 170, 200, 255);
        case 2: return IM_COL32(110, 200, 110, 255);
        case 3: return IM_COL32(230, 210, 90, 255);
        case 4: return IM_COL32(230, 130, 190, 255);
        case 5: return IM_COL32(180, 120, 240, 255);
        default: return IM_COL32(220, 220, 220, 255);
        }
    }
    int ParamDims(const std::string& t) { return t == "float" ? 1 : t == "vec2" ? 2 : (t == "vec3" || t == "color") ? 3 : t == "vec4" ? 4 : 5; }
    std::string Short(float f) { char b[32]; std::snprintf(b, sizeof(b), "%.3g", f); return b; }
    std::string ValueText(const json& v) {
        if (v.is_number()) return Short(v.get<float>());
        if (!v.is_array()) return "";
        std::string s = "(";
        for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + (v[i].is_number() ? Short(v[i].get<float>()) : std::string("?"));
        return s + ")";
    }
    std::string GraphJsonForCompile(const ShaderGraph& g) {      // positions don't change the shader
        json j = g.ToJson();
        for (auto& n : j["nodes"]) n.erase("pos");
        return j.dump();
    }
    bool Compatible(int a, int b) { return (a == 5) == (b == 5); }
}

ShaderGraphEditor::ShaderGraphEditor(std::string engineDir, std::string assetDir)
    : m_EngineDir(engineDir), m_AssetDir(assetDir), m_Preview(std::move(engineDir), std::move(assetDir)) {
    m_Preview.Closable = false;
}

bool ShaderGraphEditor::IsDirty() const { return !m_Path.empty() && m_Graph.ToJson().dump() != m_SavedText; }

void ShaderGraphEditor::Open(const std::string& path) {
    if (path == m_Path) return;
    if (IsDirty()) { m_PendingOpen = path; m_AskSwitch = true; return; }
    DoOpen(path);
}

void ShaderGraphEditor::New(const std::string& path) {
    std::ofstream out(fs::u8path(m_AssetDir + path), std::ios::binary | std::ios::trunc);
    out << ShaderGraph::Default().ToJson().dump(2) << "\n";
    out.close();
    Open(path);
}

void ShaderGraphEditor::DoOpen(const std::string& path) {
    m_Previews.Clear();
    m_Path = path;
    m_Graph = ShaderGraph::Default();
    try {
        std::ifstream in(fs::u8path(m_AssetDir + path), std::ios::binary);
        if (in) m_Graph = ShaderGraph::FromJson(json::parse(in));
        else m_Status = "No such file: saving creates it";
    }
    catch (const std::exception& e) { m_Status = std::string("Can't read it: ") + e.what(); }
    m_SavedText = m_Graph.ToJson().dump();
    m_Undo = { m_SavedText };
    m_UndoPos = 0;
    m_Selected.clear();
    m_FrameNext = 3;
    m_PublishedText.clear();
    Publish();
    if (m_Resources) m_Preview.SetAsset(m_Resources->GraphPreviewMaterial(path));
    m_Preview.Title = fs::u8path(path).filename().string();
}

bool ShaderGraphEditor::Save() {
    if (m_Path.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::u8path(m_AssetDir + m_Path).parent_path(), ec);
    std::ofstream out(fs::u8path(m_AssetDir + m_Path), std::ios::binary | std::ios::trunc);
    if (!out) { m_Status = "Can't write " + m_Path; return false; }
    out << m_Graph.ToJson().dump(2) << "\n";
    out.close();
    m_SavedText = m_Graph.ToJson().dump();
    m_Status = "Saved " + m_Path;
    return true;
}

void ShaderGraphEditor::Revert() {
    const std::string p = m_Path;
    m_Path.clear();
    DoOpen(p);
    m_Status = "Read " + p + " again";
}

void ShaderGraphEditor::Render(float dt) {
    if (!m_Resources) return;
    m_Preview.Render(dt, *m_Resources);
    m_Time += dt;
    const bool visible = m_Visible;
    m_Visible = false;
    if (!visible || !m_ShowPreviews || m_Path.empty()) return;
    m_Previews.Update(m_Graph, std::hash<std::string>()(m_PublishedText), m_Time, [this](const SGParam& p) -> std::shared_ptr<Texture2D> {
        if (p.Texture.empty()) return nullptr;
        if (p.Texture == "checker") return m_Resources->Checker();
        return p.Linear ? m_Resources->LoadDataTexture(p.Texture) : m_Resources->LoadTexture(p.Texture);
    });
}

// ---------------------------------------------------------------- edits
void ShaderGraphEditor::Publish() {
    m_PublishedText = GraphJsonForCompile(m_Graph);
    m_Compiled = CompileShaderGraph(m_Graph);
    m_GlslError.clear();
    if (m_Resources && !m_Path.empty()) m_Resources->SetShaderGraph(m_Path, m_Graph);
    if (m_Compiled.Ok) {                                   // the driver's verdict (compiles now: we have the context)
        auto program = SGProgram::For(m_Compiled.Code, m_Compiled.VertexCode);
        if (!program->Get(0)) m_GlslError = program->Error();
        else if ((m_Compiled.WritesAlpha || m_Compiled.HasVertexOffset) && (!program->Get(1) || !program->Get(2) || !program->Get(3))) m_GlslError = program->Error();
    }
}

void ShaderGraphEditor::Changed() {
    if (GraphJsonForCompile(m_Graph) != m_PublishedText) Publish();
}

void ShaderGraphEditor::Commit() {
    const std::string now = m_Graph.ToJson().dump();
    if (m_UndoPos >= 0 && m_UndoPos < (int)m_Undo.size() && m_Undo[m_UndoPos] == now) return;
    m_Undo.resize(m_UndoPos + 1);
    m_Undo.push_back(now);
    if (m_Undo.size() > 200) m_Undo.erase(m_Undo.begin());
    m_UndoPos = (int)m_Undo.size() - 1;
}

void ShaderGraphEditor::Undo() {
    if (m_UndoPos <= 0) return;
    m_Graph = ShaderGraph::FromJson(json::parse(m_Undo[--m_UndoPos]));
    m_Selected.clear();
    Changed();
}

void ShaderGraphEditor::Redo() {
    if (m_UndoPos + 1 >= (int)m_Undo.size()) return;
    m_Graph = ShaderGraph::FromJson(json::parse(m_Undo[++m_UndoPos]));
    m_Selected.clear();
    Changed();
}

int ShaderGraphEditor::AddNode(const std::string& type, const glm::vec2& pos) {
    SGNode n;
    n.Id = m_Graph.NextId();
    n.Type = type;
    n.Pos = pos;
    if (type == "Property" && !m_Graph.Params.empty()) n.Values["param"] = m_Graph.Params.front().Name;
    if (type == "Color") n.Values["Color"] = json::array({ 1.0, 1.0, 1.0, 1.0 });
    m_Graph.Nodes.push_back(n);
    m_Selected = { n.Id };
    Changed();
    return n.Id;
}

void ShaderGraphEditor::Connect(const PinRef& a, const PinRef& b) {
    if (a.Output == b.Output || a.Node == b.Node || !Compatible(a.Type, b.Type)) return;
    const PinRef& out = a.Output ? a : b;
    const PinRef& in = a.Output ? b : a;
    std::erase_if(m_Graph.Links, [&](const SGLink& l) { return l.ToNode == in.Node && l.ToPin == in.Pin; });
    m_Graph.Links.push_back({ out.Node, out.Pin, in.Node, in.Pin });
    Changed();
}

void ShaderGraphEditor::Disconnect(int node, const std::string& pin) {
    std::erase_if(m_Graph.Links, [&](const SGLink& l) { return l.ToNode == node && l.ToPin == pin; });
    Changed();
}

void ShaderGraphEditor::DeleteSelected() {
    std::set<int> gone;
    for (int id : m_Selected) if (const SGNode* n = m_Graph.Find(id); n && n->Type != "Output") gone.insert(id);
    std::erase_if(m_Graph.Nodes, [&](const SGNode& n) { return gone.count(n.Id) > 0; });
    std::erase_if(m_Graph.Links, [&](const SGLink& l) { return gone.count(l.FromNode) || gone.count(l.ToNode); });
    m_Selected.clear();
    Changed();
}

void ShaderGraphEditor::DuplicateSelected() {
    std::map<int, int> copies;
    for (int id : m_Selected) {
        const SGNode* n = m_Graph.Find(id);
        if (!n || n->Type == "Output") continue;
        SGNode c = *n;
        c.Id = m_Graph.NextId();
        c.Pos += glm::vec2(30.0f, 30.0f);
        copies[id] = c.Id;
        m_Graph.Nodes.push_back(c);
    }
    // links between copied nodes are copied too
    const auto links = m_Graph.Links;
    for (const SGLink& l : links)
        if (copies.count(l.FromNode) && copies.count(l.ToNode))
            m_Graph.Links.push_back({ copies[l.FromNode], l.FromPin, copies[l.ToNode], l.ToPin });
    m_Selected.clear();
    for (auto& [from, to] : copies) m_Selected.insert(to);
    Changed();
}

std::string ShaderGraphEditor::UniqueParamName(const std::string& base) const {
    for (int i = 1;; i++) {
        const std::string name = i == 1 ? base : base + std::to_string(i);
        if (!m_Graph.Param(name)) return name;
    }
}

// ---------------------------------------------------------------- canvas geometry
ImVec2 ShaderGraphEditor::ToScreen(const glm::vec2& p) const {
    return ImVec2(m_Origin.x + m_Size.x * 0.5f + (p.x + m_Pan.x) * m_Zoom, m_Origin.y + m_Size.y * 0.5f + (p.y + m_Pan.y) * m_Zoom);
}
glm::vec2 ShaderGraphEditor::ToCanvas(const ImVec2& p) const {
    return glm::vec2((p.x - m_Origin.x - m_Size.x * 0.5f) / m_Zoom - m_Pan.x, (p.y - m_Origin.y - m_Size.y * 0.5f) / m_Zoom - m_Pan.y);
}

float ShaderGraphEditor::NodeWidth(const SGNode& n) const {
    const SGNodeDef* d = ShaderGraphNode(n.Type);
    if (!d) return 140.0f;
    float in = 0.0f, out = 0.0f;
    for (const SGPinDef& p : d->In) in = std::max(in, ImGui::CalcTextSize(p.Name.c_str()).x + 46.0f);
    for (const SGPinDef& p : ShaderGraphOutputs(m_Graph, n)) out = std::max(out, ImGui::CalcTextSize(p.Name.c_str()).x);
    const std::string title = n.Type == "Property" ? n.Values.value("param", std::string("Property")) : n.Type;
    return std::max({ ShowsPreview(n) ? (float)SGNodePreviews::Size + 2.0f * kPad : 130.0f, 130.0f, ImGui::CalcTextSize(title.c_str()).x + 24.0f, in + out + 36.0f });
}

bool ShaderGraphEditor::ShowsPreview(const SGNode& n) const { return m_ShowPreviews && SGNodePreviews::Previewable(m_Graph, n); }

float ShaderGraphEditor::NodeHeight(const SGNode& n) const {
    const SGNodeDef* d = ShaderGraphNode(n.Type);
    const int rows = d ? (int)std::max(d->In.size(), ShaderGraphOutputs(m_Graph, n).size()) : 1;
    return kHeader + rows * kRow + kPad + (ShowsPreview(n) ? (float)SGNodePreviews::Size + kPad : 0.0f);
}

ImVec2 ShaderGraphEditor::PinPos(const SGNode& n, int index, bool output) const {
    const float x = output ? n.Pos.x + NodeWidth(n) : n.Pos.x;
    return ToScreen(glm::vec2(x, n.Pos.y + kHeader + (index + 0.5f) * kRow));
}

bool ShaderGraphEditor::PinAt(const ImVec2& m, PinRef& out) const {
    const float r = std::max(kPinR * m_Zoom, 4.0f) + 3.0f;
    for (const SGNode& n : m_Graph.Nodes) {
        const SGNodeDef* d = ShaderGraphNode(n.Type);
        if (!d) continue;
        for (int i = 0; i < (int)d->In.size(); i++) {
            const ImVec2 p = PinPos(n, i, false);
            if (std::abs(m.x - p.x) <= r && std::abs(m.y - p.y) <= r) { out = { n.Id, d->In[i].Name, false, d->In[i].Type }; return true; }
        }
        const auto outs = ShaderGraphOutputs(m_Graph, n);
        for (int i = 0; i < (int)outs.size(); i++) {
            const ImVec2 p = PinPos(n, i, true);
            if (std::abs(m.x - p.x) <= r && std::abs(m.y - p.y) <= r) { out = { n.Id, outs[i].Name, true, outs[i].Type }; return true; }
        }
    }
    return false;
}

int ShaderGraphEditor::NodeAt(const ImVec2& m) const {
    for (int i = (int)m_Graph.Nodes.size() - 1; i >= 0; i--) {
        const SGNode& n = m_Graph.Nodes[i];
        const SGNodeDef* d = ShaderGraphNode(n.Type);
        const int rows = d ? (int)std::max(d->In.size(), ShaderGraphOutputs(m_Graph, n).size()) : 1;
        const ImVec2 a = ToScreen(n.Pos), b = ToScreen(n.Pos + glm::vec2(NodeWidth(n), NodeHeight(n)));
        if (m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y) return n.Id;
    }
    return 0;
}

void ShaderGraphEditor::FrameAll() {
    if (m_Graph.Nodes.empty() || m_Size.x < 10.0f) return;
    glm::vec2 mn(1e9f), mx(-1e9f);
    for (const SGNode& n : m_Graph.Nodes) {
        mn = glm::min(mn, n.Pos);
        const SGNodeDef* d = ShaderGraphNode(n.Type);
        const int rows = d ? (int)std::max(d->In.size(), ShaderGraphOutputs(m_Graph, n).size()) : 1;
        mx = glm::max(mx, n.Pos + glm::vec2(NodeWidth(n), NodeHeight(n)));
    }
    const glm::vec2 size = mx - mn + glm::vec2(60.0f);
    m_Zoom = std::clamp(std::min(m_Size.x / size.x, (m_Size.y - 30.0f) / size.y), 0.3f, 1.25f);   // (room for the hint line)
    m_Pan = -(mn + mx) * 0.5f;
}

// ---------------------------------------------------------------- drawing the canvas
void ShaderGraphEditor::DrawNode(ImDrawList* dl, const SGNode& n) {
    const SGNodeDef* d = ShaderGraphNode(n.Type);
    const auto outs = ShaderGraphOutputs(m_Graph, n);
    const int rows = d ? (int)std::max(d->In.size(), outs.size()) : 1;
    const float w = NodeWidth(n);
    const ImVec2 a = ToScreen(n.Pos), b = ToScreen(n.Pos + glm::vec2(w, NodeHeight(n)));
    const float round = 5.0f * m_Zoom, fs = ImGui::GetFontSize() * m_Zoom;
    ImFont* font = ImGui::GetFont();
    const bool selected = m_Selected.count(n.Id) > 0;
    const auto err = m_Compiled.NodeErrors.find(n.Id);

    dl->AddRectFilled(a, b, IM_COL32(36, 38, 44, 240), round);
    const ImVec2 hb(b.x, a.y + kHeader * m_Zoom);
    dl->AddRectFilled(a, hb, d ? CategoryColor(d->Category) : IM_COL32(120, 40, 40, 255), round, ImDrawFlags_RoundCornersTop);
    std::string title = n.Type;
    if (n.Type == "Property") title = n.Values.value("param", std::string("(no param)"));
    if (fs > 5.0f) dl->AddText(font, fs, ImVec2(a.x + 8.0f * m_Zoom, a.y + (kHeader * m_Zoom - fs) * 0.5f), IM_COL32(240, 240, 240, 255), title.c_str());
    ImU32 border = selected ? IM_COL32(255, 200, 80, 255) : IM_COL32(15, 15, 18, 255);
    if (err != m_Compiled.NodeErrors.end()) border = IM_COL32(240, 70, 60, 255);
    dl->AddRect(a, b, border, round, 0, selected || err != m_Compiled.NodeErrors.end() ? 2.0f : 1.0f);
    if (!d) return;

    const float pr = std::max(kPinR * m_Zoom, 2.5f);
    for (int i = 0; i < (int)d->In.size(); i++) {
        const SGPinDef& p = d->In[i];
        const ImVec2 c = PinPos(n, i, false);
        bool linked = false;
        for (const SGLink& l : m_Graph.Links) if (l.ToNode == n.Id && l.ToPin == p.Name) { linked = true; break; }
        if (linked) dl->AddCircleFilled(c, pr, TypeColor(p.Type));
        else dl->AddCircle(c, pr, TypeColor(p.Type), 0, 1.5f);
        if (fs < 5.0f) continue;
        std::string label = p.Name;
        if (!linked && p.Type != 5) {
            if (auto it = n.Values.find(p.Name); it != n.Values.end()) label += "  " + ValueText(*it);
            else if (!p.Expr.empty()) label += "  " + std::string(p.Expr == "IN.UV" ? "UV" : p.Expr);
        }
        else if (!linked && p.Type == 5) {
            const std::string img = n.Values.value("texture", std::string());
            if (!img.empty()) label += "  " + fs::u8path(img).filename().string();
        }
        dl->AddText(font, fs, ImVec2(c.x + 9.0f * m_Zoom, c.y - fs * 0.5f), linked ? IM_COL32(225, 225, 225, 255) : IM_COL32(160, 160, 165, 255), label.c_str());
    }
    for (int i = 0; i < (int)outs.size(); i++) {
        const ImVec2 c = PinPos(n, i, true);
        dl->AddCircleFilled(c, pr, TypeColor(outs[i].Type));
        if (fs < 5.0f) continue;
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, outs[i].Name.c_str());
        dl->AddText(font, fs, ImVec2(c.x - 9.0f * m_Zoom - ts.x, c.y - fs * 0.5f), IM_COL32(225, 225, 225, 255), outs[i].Name.c_str());
    }
    // a Color constant shows its colour
    // its thumbnail (SGNodePreviews): the first output, under the pins
    if (ShowsPreview(n)) {
        const float top = n.Pos.y + kHeader + rows * kRow + kPad * 0.5f;
        const float left = n.Pos.x + (w - (float)SGNodePreviews::Size) * 0.5f;
        const ImVec2 pa = ToScreen(glm::vec2(left, top)), pb = ToScreen(glm::vec2(left + SGNodePreviews::Size, top + SGNodePreviews::Size));
        if (const uint32_t t = m_Previews.Texture(n.Id)) dl->AddImage((ImTextureID)(intptr_t)t, pa, pb, ImVec2(0, 1), ImVec2(1, 0));
        else dl->AddRectFilled(pa, pb, IM_COL32(20, 20, 24, 255));
        dl->AddRect(pa, pb, IM_COL32(10, 10, 12, 255));
    }
    if (n.Type == "Color" && !ShowsPreview(n)) {
        glm::vec4 col(1.0f);
        if (auto it = n.Values.find("Color"); it != n.Values.end() && it->is_array())
            for (size_t k = 0; k < it->size() && k < 4; k++) col[(int)k] = (*it)[k].get<float>();
        dl->AddRectFilled(ImVec2(a.x + 8.0f * m_Zoom, b.y - (kPad + kRow - 4.0f) * m_Zoom), ImVec2(a.x + 40.0f * m_Zoom, b.y - kPad * m_Zoom),
            ImGui::ColorConvertFloat4ToU32(ImVec4(col.r, col.g, col.b, 1.0f)), 3.0f);
    }
}

void ShaderGraphEditor::DrawCanvas() {
    ImGuiIO& io = ImGui::GetIO();
    m_Origin = ImGui::GetCursorScreenPos();
    m_Size = ImGui::GetContentRegionAvail();
    if (m_Size.x < 50.0f || m_Size.y < 50.0f) return;
    if (m_FrameNext > 0) { FrameAll(); m_FrameNext--; }
    ImGui::InvisibleButton("##sgcanvas", m_Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(m_Origin.x + m_Size.x, m_Origin.y + m_Size.y);
    dl->PushClipRect(m_Origin, end, true);
    dl->AddRectFilled(m_Origin, end, IM_COL32(24, 25, 29, 255));

    // grid
    for (float step : { 20.0f, 100.0f }) {
        const float s = step * m_Zoom;
        if (s < 8.0f) continue;
        const ImU32 col = step > 50.0f ? IM_COL32(50, 52, 58, 255) : IM_COL32(34, 35, 40, 255);
        const ImVec2 o = ToScreen(glm::vec2(0.0f));
        for (float x = std::fmod(o.x - m_Origin.x, s); x < m_Size.x; x += s) dl->AddLine(ImVec2(m_Origin.x + x, m_Origin.y), ImVec2(m_Origin.x + x, end.y), col);
        for (float y = std::fmod(o.y - m_Origin.y, s); y < m_Size.y; y += s) dl->AddLine(ImVec2(m_Origin.x, m_Origin.y + y), ImVec2(end.x, m_Origin.y + y), col);
    }

    // links
    auto bezier = [&](ImVec2 p0, ImVec2 p1, ImU32 col) {
        const float dx = std::max(std::abs(p1.x - p0.x) * 0.5f, 40.0f * m_Zoom);
        dl->AddBezierCubic(p0, ImVec2(p0.x + dx, p0.y), ImVec2(p1.x - dx, p1.y), p1, col, std::max(2.0f * m_Zoom, 1.5f));
    };
    for (const SGLink& l : m_Graph.Links) {
        const SGNode* from = m_Graph.Find(l.FromNode);
        const SGNode* to = m_Graph.Find(l.ToNode);
        const SGNodeDef* td = to ? ShaderGraphNode(to->Type) : nullptr;
        if (!from || !td) continue;
        const auto outs = ShaderGraphOutputs(m_Graph, *from);
        int oi = -1, ii = -1;
        for (int i = 0; i < (int)outs.size(); i++) if (outs[i].Name == l.FromPin) oi = i;
        for (int i = 0; i < (int)td->In.size(); i++) if (td->In[i].Name == l.ToPin) ii = i;
        if (oi < 0 || ii < 0) continue;
        bezier(PinPos(*from, oi, true), PinPos(*to, ii, false), TypeColor(outs[oi].Type));
    }

    for (const SGNode& n : m_Graph.Nodes) DrawNode(dl, n);

    // ---- interaction
    const ImVec2 mouse = io.MousePos;
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (hovered && io.MouseWheel != 0.0f) {                 // zoom about the pointer
        const glm::vec2 before = ToCanvas(mouse);
        m_Zoom = std::clamp(m_Zoom * std::pow(1.15f, io.MouseWheel), 0.25f, 2.5f);
        const glm::vec2 after = ToCanvas(mouse);
        m_Pan += after - before;
    }
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt && !PinAt(mouse, m_LinkFrom))))
        m_Panning = true;
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)) m_Panning = true;
    if (m_Panning) {
        m_Pan += glm::vec2(io.MouseDelta.x, io.MouseDelta.y) / m_Zoom;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            m_Panning = false;
    }

    PinRef pin;
    if (hovered && !m_Panning && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
        if (PinAt(mouse, pin)) {
            if (!pin.Output) {                                 // grabbing a linked input picks up its link
                for (const SGLink& l : m_Graph.Links)
                    if (l.ToNode == pin.Node && l.ToPin == pin.Pin) {
                        const SGNode* from = m_Graph.Find(l.FromNode);
                        int type = 1;
                        if (from) for (const auto& o : ShaderGraphOutputs(m_Graph, *from)) if (o.Name == l.FromPin) type = o.Type;
                        PinRef src{ l.FromNode, l.FromPin, true, type };
                        Disconnect(pin.Node, pin.Pin);
                        pin = src;
                        break;
                    }
            }
            m_Linking = true;
            m_LinkFrom = pin;
        }
        else if (const int id = NodeAt(mouse)) {
            if (io.KeyCtrl) { if (m_Selected.count(id)) m_Selected.erase(id); else m_Selected.insert(id); }
            else if (!m_Selected.count(id)) m_Selected = { id };
            m_Dragging = id;
            // the node clicked comes to the front
            auto it = std::find_if(m_Graph.Nodes.begin(), m_Graph.Nodes.end(), [&](const SGNode& n) { return n.Id == id; });
            if (it != m_Graph.Nodes.end()) { SGNode n = *it; m_Graph.Nodes.erase(it); m_Graph.Nodes.push_back(n); }
        }
        else {
            if (!io.KeyCtrl) m_Selected.clear();
            m_BoxSelecting = true;
            m_BoxStart = mouse;
        }
    }
    // Alt+click or right-click on an input pin: disconnect
    if (hovered && ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt) || ImGui::IsMouseReleased(ImGuiMouseButton_Right)) &&
        PinAt(mouse, pin) && !pin.Output && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right, 4.0f)) {
        Disconnect(pin.Node, pin.Pin);
        m_Panning = false;
    }
    else if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right, 4.0f)) {
        m_OpenAddMenu = true;                                // right-click on empty space: add a node
        m_AddAt = ToCanvas(mouse);
        m_AddConnect = false;
    }

    if (m_Dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        const glm::vec2 d(io.MouseDelta.x / m_Zoom, io.MouseDelta.y / m_Zoom);
        for (int id : m_Selected) if (SGNode* n = m_Graph.Find(id)) n->Pos += d;
    }
    if (m_Linking) {
        const ImVec2 from = [&] {
            const SGNode* n = m_Graph.Find(m_LinkFrom.Node);
            const SGNodeDef* d = n ? ShaderGraphNode(n->Type) : nullptr;
            if (!n || !d) return mouse;
            if (m_LinkFrom.Output) { const auto o = ShaderGraphOutputs(m_Graph, *n); for (int i = 0; i < (int)o.size(); i++) if (o[i].Name == m_LinkFrom.Pin) return PinPos(*n, i, true); }
            else for (int i = 0; i < (int)d->In.size(); i++) if (d->In[i].Name == m_LinkFrom.Pin) return PinPos(*n, i, false);
            return mouse;
        }();
        if (m_LinkFrom.Output) bezier(from, mouse, TypeColor(m_LinkFrom.Type));
        else bezier(mouse, from, TypeColor(m_LinkFrom.Type));
    }
    if (m_BoxSelecting) {
        dl->AddRectFilled(m_BoxStart, mouse, IM_COL32(255, 200, 80, 30));
        dl->AddRect(m_BoxStart, mouse, IM_COL32(255, 200, 80, 150));
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_Linking) {
            m_Linking = false;
            if (PinAt(mouse, pin)) Connect(m_LinkFrom, pin);
            else if (hovered && !NodeAt(mouse)) { m_OpenAddMenu = true; m_AddAt = ToCanvas(mouse); m_AddConnect = true; }   // a new node, connected
        }
        if (m_BoxSelecting) {
            m_BoxSelecting = false;
            const glm::vec2 a = ToCanvas(ImVec2(std::min(m_BoxStart.x, mouse.x), std::min(m_BoxStart.y, mouse.y)));
            const glm::vec2 b = ToCanvas(ImVec2(std::max(m_BoxStart.x, mouse.x), std::max(m_BoxStart.y, mouse.y)));
            for (const SGNode& n : m_Graph.Nodes)
                if (n.Pos.x + NodeWidth(n) >= a.x && n.Pos.x <= b.x && n.Pos.y + kHeader >= a.y && n.Pos.y <= b.y) m_Selected.insert(n.Id);
        }
        m_Dragging = 0;
    }

    // a param dragged from the side panel: its Property node
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kParamPayload)) {
            const std::string name((const char*)p->Data, p->DataSize);
            const int id = AddNode("Property", ToCanvas(mouse));
            if (SGNode* n = m_Graph.Find(id)) n->Values["param"] = name;
            Changed();
        }
        ImGui::EndDragDropTarget();
    }

    // node tooltips: help and errors
    if (hovered && !m_Linking && !m_Dragging && !m_Panning)
        if (const int id = NodeAt(mouse)) {
            const SGNode* n = m_Graph.Find(id);
            const SGNodeDef* d = n ? ShaderGraphNode(n->Type) : nullptr;
            const auto err = m_Compiled.NodeErrors.find(id);
            if (n && ImGui::BeginTooltip()) {
                ImGui::TextUnformatted(n->Type.c_str());
                if (d && !d->Help.empty()) { ImGui::PushTextWrapPos(320.0f); ImGui::TextDisabled("%s", d->Help.c_str()); ImGui::PopTextWrapPos(); }
                if (err != m_Compiled.NodeErrors.end()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", err->second.c_str());
                ImGui::EndTooltip();
            }
        }

    // keys
    if (focused && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) DeleteSelected();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) DuplicateSelected();
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) FrameAll();
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_Space, false)) { m_OpenAddMenu = true; m_AddAt = ToCanvas(mouse); m_AddConnect = false; }
    }

    dl->PopClipRect();
    // hint
    dl->AddText(ImVec2(m_Origin.x + 8.0f, end.y - ImGui::GetFontSize() - 6.0f), IM_COL32(120, 120, 128, 255),
        "Right-click or Space: add a node.  Drag pins to connect.  Middle / right drag: pan.  Wheel: zoom.  F: frame.");
    DrawAddMenu();
}

void ShaderGraphEditor::DrawAddMenu() {
    if (m_OpenAddMenu) { ImGui::OpenPopup("##sgadd"); m_OpenAddMenu = false; m_Search[0] = '\0'; }
    if (!ImGui::BeginPopup("##sgadd")) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(220.0f);
    const bool enter = ImGui::InputTextWithHint("##search", "Search nodes", m_Search, sizeof(m_Search), ImGuiInputTextFlags_EnterReturnsTrue);
    std::string chosen;
    std::string q = m_Search;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    auto fits = [&](const SGNodeDef& d) {
        if (d.Type == "Output") return false;
        if (m_AddConnect) {                                  // only nodes that can take the dragged link
            bool ok = false;
            if (m_LinkFrom.Output) { for (const auto& p : d.In) if (Compatible(p.Type, m_LinkFrom.Type)) ok = true; }
            else for (const auto& p : d.Out) if (Compatible(p.Type, m_LinkFrom.Type) || d.Type == "Property") ok = true;
            if (!ok) return false;
        }
        if (q.empty()) return true;
        std::string t = d.Type + " " + d.Category;
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return t.find(q) != std::string::npos;
    };
    if (!q.empty()) {
        bool first = true;
        for (const SGNodeDef& d : ShaderGraphNodes()) {
            if (!fits(d)) continue;
            if (ImGui::Selectable((d.Type + "##add").c_str()) || (enter && first)) chosen = d.Type;
            if (ImGui::IsItemHovered() && !d.Help.empty()) ImGui::SetTooltip("%s", d.Help.c_str());
            first = false;
        }
    }
    else {
        for (const char* cat : { "Input", "Texture", "UV", "Math", "Vector", "Procedural", "Surface", "Logic" }) {
            bool any = false;
            for (const SGNodeDef& d : ShaderGraphNodes()) if (d.Category == cat && fits(d)) any = true;
            if (!any || !ImGui::BeginMenu(cat)) continue;
            for (const SGNodeDef& d : ShaderGraphNodes()) {
                if (d.Category != cat || !fits(d)) continue;
                if (ImGui::MenuItem(d.Type.c_str())) chosen = d.Type;
                if (ImGui::IsItemHovered() && !d.Help.empty()) ImGui::SetTooltip("%s", d.Help.c_str());
            }
            ImGui::EndMenu();
        }
    }
    if (!chosen.empty()) {
        const int id = AddNode(chosen, m_AddAt);
        if (m_AddConnect)                                    // hook the dragged link onto the first pin that fits
            if (const SGNode* n = m_Graph.Find(id)) {
                const SGNodeDef* d = ShaderGraphNode(n->Type);
                if (m_LinkFrom.Output) {
                    for (const auto& p : d->In) if (Compatible(p.Type, m_LinkFrom.Type)) { Connect(m_LinkFrom, { id, p.Name, false, p.Type }); break; }
                }
                else {
                    SGNode moved = *n;
                    for (const auto& p : ShaderGraphOutputs(m_Graph, moved))
                        if (Compatible(p.Type, m_LinkFrom.Type)) { Connect({ id, p.Name, true, p.Type }, m_LinkFrom); break; }
                    if (SGNode* nn = m_Graph.Find(id)) nn->Pos.x -= NodeWidth(*nn) + 20.0f;   // to the left of the input
                }
            }
        m_AddConnect = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- side panel
bool ShaderGraphEditor::ImagePicker(const char* label, std::string& path) {
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::GetTime() - m_LastScan > 2.0) {
        m_LastScan = ImGui::GetTime();
        m_Images.clear();
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(fs::u8path(m_AssetDir), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string rel = fs::relative(it->path(), fs::u8path(m_AssetDir), ec).generic_string();
            if (AssetBrowser::KindOf(rel) == AssetBrowser::Kind::Texture) m_Images.push_back(rel);
        }
        std::sort(m_Images.begin(), m_Images.end());
    }
    const float thumb = ImGui::GetFrameHeight();
    const uint32_t tex = !path.empty() && TextureId ? TextureId(path) : 0;
    if (tex) ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(thumb, thumb), ImVec2(0, 1), ImVec2(1, 0));
    else ImGui::Dummy(ImVec2(thumb, thumb));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::CalcItemWidth() - thumb - ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::BeginCombo("##img", path.empty() ? "(none)" : path.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("(none)", path.empty())) { path.clear(); changed = true; }
        if (ImGui::Selectable("checker", path == "checker")) { path = "checker"; changed = true; }
        for (const auto& p : m_Images) if (ImGui::Selectable(p.c_str(), p == path)) { path = p; changed = true; }
        ImGui::EndCombo();
    }
    if (const std::string drop = AssetBrowser::AcceptDrop({ AssetBrowser::Kind::Texture }); !drop.empty()) { path = drop; changed = true; }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

bool ShaderGraphEditor::ValueEditor(const char* label, json& value, int dims) {
    float v[4] = { 0, 0, 0, 0 };
    if (value.is_number()) v[0] = value.get<float>();
    else if (value.is_array()) for (size_t i = 0; i < value.size() && i < 4; i++) if (value[i].is_number()) v[i] = value[i].get<float>();
    bool changed = false;
    switch (dims) {
    case 1: changed = ImGui::DragFloat(label, v, 0.01f); break;
    case 2: changed = ImGui::DragFloat2(label, v, 0.01f); break;
    case 3: changed = ImGui::DragFloat3(label, v, 0.01f); break;
    default: changed = ImGui::DragFloat4(label, v, 0.01f); break;
    }
    if (changed) {
        if (dims == 1) value = json::array({ v[0] });
        else { value = json::array(); for (int i = 0; i < dims; i++) value.push_back(v[i]); }
    }
    return changed;
}

void ShaderGraphEditor::DrawParams() {
    ImGui::SeparatorText("Params");
    ImGui::SetItemTooltip("The values materials using this graph set (Material Editor). Drag one onto the canvas for its node.");
    int remove = -1;
    for (int i = 0; i < (int)m_Graph.Params.size(); i++) {
        SGParam& p = m_Graph.Params[i];
        ImGui::PushID(i);
        // the handle: drag onto the canvas
        ImGui::Button("::");
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kParamPayload, p.Name.data(), p.Name.size());
            ImGui::Text("%s", p.Name.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::SetItemTooltip("Drag onto the canvas");
        ImGui::SameLine();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", p.Name.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        if (ImGui::InputText("##name", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) || (ImGui::IsItemDeactivatedAfterEdit())) {
            std::string name = buf;
            if (!name.empty() && name != p.Name && !m_Graph.Param(name) && name[0] != '_') {
                for (SGNode& n : m_Graph.Nodes)
                    if (n.Type == "Property" && n.Values.value("param", std::string()) == p.Name) n.Values["param"] = name;
                p.Name = name;
                Changed();
            }
        }
        ImGui::SameLine();
        static const char* kTypes[] = { "float", "vec2", "vec3", "vec4", "color", "texture" };
        int t = 0;
        for (int k = 0; k < 6; k++) if (p.Type == kTypes[k]) t = k;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Combo("##type", &t, kTypes, 6)) {
            p.Type = kTypes[t];
            if (p.Type == "color") p.Default = glm::vec4(1.0f);
            Changed();
        }
        ImGui::SameLine();
        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) remove = i;
        ImGui::SetItemTooltip("Remove the param (its nodes report it)");
        // its default
        ImGui::Indent();
        bool changed = false;
        if (p.Type == "texture") {
            changed |= ImagePicker("Default", p.Texture);
            changed |= ImGui::Checkbox("Data (linear)", &p.Linear);
            ImGui::SetItemTooltip("On for normal maps, roughness, masks: values, not colours (no sRGB decoding)");
        }
        else if (p.Type == "color") changed = ImGui::ColorEdit3("Default", &p.Default.x);
        else if (p.Type == "float") changed = ImGui::DragFloat("Default", &p.Default.x, 0.01f);
        else if (p.Type == "vec2") changed = ImGui::DragFloat2("Default", &p.Default.x, 0.01f);
        else if (p.Type == "vec3") changed = ImGui::DragFloat3("Default", &p.Default.x, 0.01f);
        else changed = ImGui::DragFloat4("Default", &p.Default.x, 0.01f);
        if (changed) Changed();
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (remove >= 0) { m_Graph.Params.erase(m_Graph.Params.begin() + remove); Changed(); }
    if (ImGui::Button("+ Param")) ImGui::OpenPopup("##addparam");
    if (ImGui::BeginPopup("##addparam")) {
        for (const char* t : { "float", "vec2", "vec3", "vec4", "color", "texture" })
            if (ImGui::MenuItem(t)) {
                SGParam p;
                p.Type = t;
                p.Name = UniqueParamName(std::string(t) == "texture" ? "Texture" : std::string(t) == "color" ? "Color" : "Value");
                if (p.Type == "color") p.Default = glm::vec4(1.0f);
                m_Graph.Params.push_back(p);
                Changed();
            }
        ImGui::EndPopup();
    }
}

void ShaderGraphEditor::DrawNodeProperties() {
    if (m_Selected.size() != 1) return;
    SGNode* n = m_Graph.Find(*m_Selected.begin());
    const SGNodeDef* d = n ? ShaderGraphNode(n->Type) : nullptr;
    if (!n || !d) return;
    ImGui::SeparatorText(n->Type.c_str());
    if (!d->Help.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::TextDisabled("%s", d->Help.c_str()); ImGui::PopTextWrapPos(); }
    bool changed = false;
    auto linked = [&](const std::string& pin) {
        for (const SGLink& l : m_Graph.Links) if (l.ToNode == n->Id && l.ToPin == pin) return true;
        return false;
    };
    if (n->Type == "Property") {
        const std::string cur = n->Values.value("param", std::string());
        if (ImGui::BeginCombo("Param", cur.empty() ? "(pick one)" : cur.c_str())) {
            for (const SGParam& p : m_Graph.Params)
                if (ImGui::Selectable(p.Name.c_str(), p.Name == cur)) { n->Values["param"] = p.Name; changed = true; }
            ImGui::EndCombo();
        }
    }
    else if (n->Type == "Float" || n->Type == "Vector2" || n->Type == "Vector3" || n->Type == "Vector4") {
        json& v = n->Values["Value"];
        if (v.is_null()) v = json::array();
        changed |= ValueEditor("Value", v, n->Type == "Float" ? 1 : n->Type[6] - '0');
    }
    else if (n->Type == "Color") {
        float c[4] = { 1, 1, 1, 1 };
        if (auto it = n->Values.find("Color"); it != n->Values.end() && it->is_array())
            for (size_t i = 0; i < it->size() && i < 4; i++) c[i] = (*it)[i].get<float>();
        if (ImGui::ColorEdit4("Color", c)) { n->Values["Color"] = json::array({ c[0], c[1], c[2], c[3] }); changed = true; }
    }
    if ((n->Type == "Sample Texture" || n->Type == "Sample Normal Map" || n->Type == "Triplanar") && !linked("Texture")) {
        std::string img = n->Values.value("texture", std::string());
        if (ImagePicker("Image", img)) { n->Values["texture"] = img; changed = true; }
        if (n->Type != "Sample Normal Map") {
            bool lin = n->Values.value("linear", false);
            if (ImGui::Checkbox("Data (linear)", &lin)) { n->Values["linear"] = lin; changed = true; }
        }
        ImGui::TextDisabled("Or connect a texture param (materials can then change it).");
    }
    // unconnected inputs: their values
    if (!d->Special || n->Type == "Output" || n->Type == "Sample Normal Map" || n->Type == "Triplanar" || n->Type == "Sample Texture") {
        for (const SGPinDef& p : d->In) {
            if (p.Type == 5 || linked(p.Name)) continue;
            ImGui::PushID(p.Name.c_str());
            const bool has = n->Values.contains(p.Name);
            if (!p.Expr.empty() && !has) {
                ImGui::TextDisabled("%s: %s", p.Name.c_str(), p.Expr == "IN.UV" ? "the mesh's UV" : p.Expr.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Set")) { n->Values[p.Name] = json::array({ 0.0, 0.0 }); changed = true; }
                ImGui::PopID();
                continue;
            }
            int dims = p.Type;
            json& v = n->Values[p.Name];
            if (v.is_null()) {                              // start from the default
                v = json::array();
                for (int i = 0; i < std::max(p.Type, 1); i++) v.push_back(p.Default[i]);
            }
            if (p.Type == 0) {                              // dynamic: as many numbers as wanted
                dims = v.is_array() ? std::clamp((int)v.size(), 1, 4) : 1;
                ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 1.6f);
                int k = dims - 1;
                if (ImGui::Combo("##dims", &k, "1\0" "2\0" "3\0" "4\0")) {
                    json nv = json::array();
                    for (int i = 0; i <= k; i++) nv.push_back(v.is_array() && i < (int)v.size() ? v[i] : (v.is_array() && !v.empty() ? v[0] : json(0.0)));
                    v = nv; dims = k + 1; changed = true;
                }
                ImGui::SetItemTooltip("How many numbers (a float, or a vector)");
                ImGui::SameLine();
            }
            if (n->Type == "Output" && (p.Name == "BaseColor" || p.Name == "Emission")) {
                float c[3] = { 0, 0, 0 };
                for (size_t i = 0; i < v.size() && i < 3; i++) c[i] = v[i].get<float>();
                if (ImGui::ColorEdit3(p.Name.c_str(), c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) { v = json::array({ c[0], c[1], c[2] }); changed = true; }
                ImGui::SetItemTooltip("Linear colour");
            }
            else changed |= ValueEditor(p.Name.c_str(), v, dims);
            if (!p.Expr.empty()) { ImGui::SameLine(); if (ImGui::SmallButton("UV")) { n->Values.erase(p.Name); changed = true; } }
            ImGui::PopID();
        }
    }
    if (auto err = m_Compiled.NodeErrors.find(n->Id); err != m_Compiled.NodeErrors.end())
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", err->second.c_str());
    if (changed) Changed();
}

void ShaderGraphEditor::DrawProblems() {
    const bool any = !m_Compiled.Errors.empty() || !m_Compiled.NodeErrors.empty() || !m_GlslError.empty();
    ImGui::SeparatorText(any ? "Problems" : "No problems");
    for (const std::string& e : m_Compiled.Errors) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", e.c_str());
    for (const auto& [id, e] : m_Compiled.NodeErrors) {
        const SGNode* n = m_Graph.Find(id);
        const std::string text = (n ? n->Type : std::string("?")) + " (" + std::to_string(id) + "): " + e;
        if (ImGui::Selectable(text.c_str())) { m_Selected = { id }; if (n) m_Pan = -n->Pos - glm::vec2(NodeWidth(*n) * 0.5f, 40.0f); }
    }
    if (!m_GlslError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "GLSL:");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(m_GlslError.c_str());
        ImGui::PopTextWrapPos();
    }
    if (any) ImGui::TextDisabled("Materials using it draw magenta until it is fixed.");
    ImGui::Checkbox("Show generated GLSL", &m_ShowCode);
    if (m_ShowCode) {
        static std::string code;
        code = m_Compiled.Code;
        ImGui::InputTextMultiline("##code", code.data(), code.size() + 1, ImVec2(-FLT_MIN, 260.0f), ImGuiInputTextFlags_ReadOnly);
    }
}

void ShaderGraphEditor::DrawSidePanel() {
    m_Preview.Draw(std::clamp(ImGui::GetContentRegionAvail().x * 0.8f, 150.0f, 280.0f) + ImGui::GetFrameHeightWithSpacing() * 2.0f);
    ImGui::SeparatorText("Settings");
    if (ImGui::Checkbox("Unlit", &m_Graph.Unlit)) Changed();
    ImGui::SetItemTooltip("Its colour only (base colour + emission), no lights or shadows");
    ImGui::TextDisabled("Alpha mode, cutoff and double-sided are the material's.");
    DrawParams();
    DrawNodeProperties();
    DrawProblems();
}

void ShaderGraphEditor::Draw(bool* open) {
    m_Focused = false;
    ImGui::SetNextWindowSize(ImVec2(1100, 700), ImGuiCond_FirstUseEver);
    const std::string title = std::string("Shader Graph") + (m_Path.empty() ? "" : " - " + fs::u8path(m_Path).filename().string()) +
                              (IsDirty() ? " *" : "") + "###ShaderGraph";
    if (!ImGui::Begin(title.c_str(), open, ImGuiWindowFlags_MenuBar)) { ImGui::End(); return; }
    m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    m_Visible = true;
    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save", "Ctrl+S", false, !m_Path.empty())) Save();
            if (ImGui::MenuItem("Revert", nullptr, false, IsDirty())) Revert();
            ImGui::Separator();
            if (ImGui::BeginMenu("Load Preset", !m_Path.empty())) {
                for (const auto& p : Presets()) {
                    if (ImGui::MenuItem(p.c_str())) LoadPreset(p);
                    ImGui::SetItemTooltip("Replace this graph with the %s preset (Ctrl+Z undoes it)", p.c_str());
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("New from Preset")) {
                for (const auto& p : Presets()) {
                    if (ImGui::MenuItem(p.c_str())) NewFromPreset(p);
                    ImGui::SetItemTooltip("A new graph in Assets/Shaders, starting as the %s preset", p.c_str());
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, m_UndoPos > 0)) Undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, m_UndoPos + 1 < (int)m_Undo.size())) Redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !m_Selected.empty())) DuplicateSelected();
            if (ImGui::MenuItem("Delete", "Delete", false, !m_Selected.empty())) DeleteSelected();
            if (ImGui::MenuItem("Frame All", "F")) FrameAll();
            ImGui::MenuItem("Node Previews", nullptr, &m_ShowPreviews);
            ImGui::EndMenu();
        }
        if (!m_Status.empty()) ImGui::TextDisabled("   %s", m_Status.c_str());
        ImGui::EndMenuBar();
    }

    if (m_Path.empty()) {
        ImGui::TextWrapped("Double-click a shader graph (.shadergraph) in Assets to edit it, or right-click a folder there > New Shader Graph.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Or start from a preset (a new graph in Assets/Shaders):");
        for (const auto& p : Presets()) { if (ImGui::Button(p.c_str(), ImVec2(160, 0))) NewFromPreset(p); }
        ImGui::End();
        return;
    }
    if (m_Focused && !io.WantTextInput && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) Redo(); else Undo(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
    }

    ImGui::BeginChild("##sgside", ImVec2(m_SideWidth, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Border);
    m_SideWidth = ImGui::GetWindowWidth();
    DrawSidePanel();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##sgcanvaschild", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas();
    ImGui::EndChild();

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) Commit();

    if (m_AskSwitch) { ImGui::OpenPopup("Unsaved shader graph"); m_AskSwitch = false; }
    if (ImGui::BeginPopupModal("Unsaved shader graph", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has unsaved changes.", m_Path.c_str());
        if (ImGui::Button("Save", ImVec2(100, 0))) { if (Save()) DoOpen(m_PendingOpen); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100, 0))) { Revert(); DoOpen(m_PendingOpen); ImGui::CloseCurrentPopup(); }   // the live copy too
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) { m_PendingOpen.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::End();
}

// ---------------------------------------------------------------- presets
std::vector<std::string> ShaderGraphEditor::Presets() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::u8path(m_EngineDir + "ShaderGraphs/Presets"), ec))
        if (e.path().extension() == ".shadergraph") out.push_back(e.path().stem().string());
    std::sort(out.begin(), out.end());
    return out;
}

void ShaderGraphEditor::LoadPreset(const std::string& name) {
    try {
        std::ifstream in(fs::u8path(m_EngineDir + "ShaderGraphs/Presets/" + name + ".shadergraph"), std::ios::binary);
        if (!in) { m_Status = "No preset " + name; return; }
        m_Graph = ShaderGraph::FromJson(json::parse(in));
    }
    catch (const std::exception& e) { m_Status = "Preset " + name + ": " + e.what(); return; }
    m_Selected.clear();
    m_FrameNext = 3;
    Changed();
    Commit();                                         // one undo step back to what was there
    m_Status = "Loaded the " + name + " preset (Ctrl+Z undoes it)";
}

void ShaderGraphEditor::NewFromPreset(const std::string& name) {
    std::string stem = name;
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::string path;
    for (int n = 1; n < 1000; n++) {
        path = "Shaders/" + stem + (n > 1 ? "_" + std::to_string(n) : "") + ".shadergraph";
        std::error_code ec;
        if (!fs::exists(fs::u8path(m_AssetDir + path), ec)) break;
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(m_AssetDir + "Shaders"), ec);
    fs::copy_file(fs::u8path(m_EngineDir + "ShaderGraphs/Presets/" + name + ".shadergraph"), fs::u8path(m_AssetDir + path), ec);
    if (ec) { m_Status = "Can't make " + path + ": " + ec.message(); return; }
    Open(path);
    m_Status = "Made " + path + " from the " + name + " preset";
}
