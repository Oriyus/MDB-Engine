#include "Runtime/UIDocument.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>

namespace Engine {

    using json = nlohmann::json;

    // ------------------------------------------------------------------ JSON
    namespace {
        json NodeToJson(const UINode& n) {
            json j = n.Props.is_object() ? n.Props : json::object();
            j["type"] = n.Type;
            if (!n.Name.empty()) j["name"] = n.Name;
            else j.erase("name");
            j.erase("children");
            if (!n.Children.empty()) {
                json c = json::array();
                for (const UINode& k : n.Children) c.push_back(NodeToJson(k));
                j["children"] = std::move(c);
            }
            return j;
        }

        bool NodeFromJson(const json& j, UINode& n, std::string* error) {
            if (!j.is_object()) { if (error) *error = "a widget is not an object"; return false; }
            n = UINode{};
            n.Type = j.value("type", std::string("label"));
            n.Name = j.contains("name") && j["name"].is_string() ? j["name"].get<std::string>() : std::string();
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.key() != "type" && it.key() != "name" && it.key() != "children") n.Props[it.key()] = it.value();
            if (auto c = j.find("children"); c != j.end() && c->is_array())
                for (const json& k : *c) {
                    UINode child;
                    if (!NodeFromJson(k, child, error)) return false;
                    n.Children.push_back(std::move(child));
                }
            return true;
        }

        template<typename F> void Walk(std::vector<UINode>& list, F&& f) {
            for (UINode& n : list) { f(n); Walk(n.Children, f); }
        }
        template<typename F> void Walk(const std::vector<UINode>& list, F&& f, int depth = 0) {
            for (const UINode& n : list) { f(n, depth); Walk(n.Children, f, depth + 1); }
        }
    }

    json UIDocument::ToJson() const {
        json j;
        j["version"] = kVersion;
        if (Preview.is_object() && !Preview.empty()) j["preview"] = Preview;
        json w = json::array();
        for (const UINode& n : Widgets) w.push_back(NodeToJson(n));
        j["widgets"] = std::move(w);
        return j;
    }

    bool UIDocument::FromJson(const json& j, UIDocument& out, std::string* error) {
        if (!j.is_object()) { if (error) *error = "not a JSON object"; return false; }
        UIDocument d;
        if (auto p = j.find("preview"); p != j.end() && p->is_object()) d.Preview = *p;
        if (auto w = j.find("widgets"); w != j.end()) {
            if (!w->is_array()) { if (error) *error = "\"widgets\" is not a list"; return false; }
            for (const json& k : *w) {
                UINode n;
                if (!NodeFromJson(k, n, error)) return false;
                d.Widgets.push_back(std::move(n));
            }
        }
        d.AssignIds();
        out = std::move(d);
        return true;
    }

    std::shared_ptr<UIDocument> UIDocument::Load(const std::string& file, std::string* error) {
        std::ifstream in(std::filesystem::u8path(file));
        if (!in) { if (error) *error = "can't open the file"; return nullptr; }
        json j = json::parse(in, nullptr, false);
        if (j.is_discarded()) { if (error) *error = "not valid JSON"; return nullptr; }
        auto doc = std::make_shared<UIDocument>();
        if (!FromJson(j, *doc, error)) return nullptr;
        return doc;
    }

    bool UIDocument::Save(const std::string& file) const {
        namespace fs = std::filesystem;
        const fs::path path = fs::u8path(file);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        const fs::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary);
            if (!out) return false;
            out << ToJson().dump(2) << "\n";
            if (!out) return false;
        }
        fs::rename(tmp, path, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
        return true;
    }

    void UIDocument::AssignIds() {
        uint32_t next = 1;
        Walk(Widgets, [&](UINode& n) { n.Id = next++; });
    }

    uint32_t UIDocument::NextId() const {
        uint32_t top = 0;
        Walk(Widgets, [&](const UINode& n, int) { top = std::max(top, n.Id); });
        return top + 1;
    }

    UINode* UIDocument::Find(uint32_t id) {
        UINode* found = nullptr;
        Walk(Widgets, [&](UINode& n) { if (n.Id == id) found = &n; });
        return found;
    }
    const UINode* UIDocument::Find(uint32_t id) const { return const_cast<UIDocument*>(this)->Find(id); }

    std::vector<std::string> UIDocument::Issues(const std::string& assetDir) const {
        static const char* kTypes[] = { "panel", "row", "column", "scroll", "label", "button", "image", "bar", "slot", "space", "separator" };
        std::vector<std::string> out;
        std::map<std::string, int> names;
        auto label = [](const UINode& n) { return n.Type + (n.Name.empty() ? std::string() : " '" + n.Name + "'"); };
        for (const UINode& n : Widgets)
            if (n.Type != "panel") out.push_back(label(n) + " is at the top: only panels can be (put it in one)");
        Walk(Widgets, [&](const UINode& n, int depth) {
            if (std::find(std::begin(kTypes), std::end(kTypes), n.Type) == std::end(kTypes)) out.push_back("unknown widget type '" + n.Type + "'");
            if (!n.Name.empty()) names[n.Name]++;
            if (n.Type == "panel" && depth > 0) out.push_back(label(n) + " is inside another widget: panels only go at the top");
            if (n.IsContainer() && n.Children.empty()) out.push_back(label(n) + " is empty");
            if (!n.IsContainer() && !n.Children.empty()) out.push_back(label(n) + " has children, which aren't drawn");
            if (n.Type == "button" && n.Name.empty() && !n.Props.contains("on_click") && !n.Props.contains("event"))
                out.push_back("button '" + n.Props.value("text", std::string()) + "' has no name, on_click or event: nothing can tell it was clicked");
            for (const char* key : { "path", "icon", "image" }) {
                auto it = n.Props.find(key);
                if (it == n.Props.end() || !it->is_string()) continue;
                const std::string p = it->get<std::string>();
                if (p.empty() || p.find("{=") != std::string::npos) continue;
                std::error_code ec;
                if (!std::filesystem::exists(std::filesystem::u8path(assetDir + p), ec))
                    out.push_back(label(n) + ": no image " + p);
            }
        });
        for (const auto& [name, count] : names)
            if (count > 1) out.push_back("the name '" + name + "' is used " + std::to_string(count) + " times (clicks report the name)");
        return out;
    }

    // ------------------------------------------------------------------ drawing
    namespace {
        bool ParseHexColor(const std::string& s, glm::vec3& out) {
            std::string h = s;
            if (!h.empty() && h[0] == '#') h = h.substr(1);
            if (h.size() != 6) return false;
            unsigned v = 0;
            for (char c : h) {
                v <<= 4;
                if (c >= '0' && c <= '9') v |= unsigned(c - '0');
                else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
                else return false;
            }
            out = glm::vec3(((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f);
            return true;
        }

        struct Walker {
            UIContext& UI;
            UIDocEvaluator& Eval;
            const std::string& Prefix;
            const std::vector<ScriptUIItem>& Items;
            std::vector<UINodeRect>* Rects;
            UIDocResult Result;

            const json* Get(const UINode& n, const char* k) const {
                auto it = n.Props.find(k);
                return it == n.Props.end() || it->is_null() ? nullptr : &*it;
            }
            std::string Raw(const UINode& n, const char* k, const std::string& d = {}) const {
                const json* v = Get(n, k);
                if (!v) return d;
                if (v->is_string()) return v->get<std::string>();
                if (v->is_number()) return v->dump();
                return d;
            }
            std::string Text(const UINode& n, const char* k) {
                const std::string s = Raw(n, k);
                return s.find("{=") != std::string::npos ? Eval.Text(s) : s;
            }
            float Num(const UINode& n, const char* k, float d) {
                const json* v = Get(n, k);
                if (!v) return d;
                if (v->is_number()) return v->get<float>();
                if (v->is_string()) {
                    const std::string s = v->get<std::string>();
                    return s.empty() ? d : Eval.Number(s, d);
                }
                return d;
            }
            bool Bool(const UINode& n, const char* k, bool d) {
                const json* v = Get(n, k);
                if (!v) return d;
                if (v->is_boolean()) return v->get<bool>();
                if (v->is_number()) return v->get<float>() != 0.0f;
                if (v->is_string()) {
                    const std::string s = v->get<std::string>();
                    return s.empty() ? d : Eval.Bool(s, d);
                }
                return d;
            }
            bool Color(const UINode& n, const char* k, glm::vec3& out) const {
                const json* v = Get(n, k);
                if (!v) return false;
                if (v->is_array() && v->size() >= 3 && (*v)[0].is_number()) {
                    out = glm::vec3((*v)[0].get<float>(), (*v)[1].get<float>(), (*v)[2].get<float>());
                    return true;
                }
                if (v->is_string()) return ParseHexColor(v->get<std::string>(), out);
                return false;
            }
            std::string Key(const UINode& n) const { return n.Name.empty() ? "#" + std::to_string(n.Id) : n.Name; }

            void Clicked(const UINode& n, int index) {
                Result.Clicked = Key(n);
                Result.Index = index;
                const std::string code = Raw(n, "on_click");
                if (!code.empty()) Eval.Run(code);
                const std::string ev = Raw(n, "event");
                if (!ev.empty()) Eval.Event(ev, n, index);
            }

            void Nodes(const std::vector<UINode>& list, int depth) {
                for (const UINode& n : list) Node(n, depth);
            }

            void Node(const UINode& n, int depth) {
                if ((depth == 0) != (n.Type == "panel")) return;     // panels at the top, everything else inside them
                const json* rep = Get(n, "repeat");
                if (!rep) { One(n, depth, 0); return; }
                const std::string expr = rep->is_string() ? rep->get<std::string>() : rep->is_number() ? rep->dump() : std::string();
                if (expr.empty()) { One(n, depth, 0); return; }
                const int count = std::clamp(Eval.BeginRepeat(expr), 0, 1000);
                for (int i = 1; i <= count; i++) {
                    Eval.SetRepeat(i);
                    One(n, depth, i);
                }
                Eval.EndRepeat();
            }

            void One(const UINode& n, int depth, int index) {
                if (Get(n, "visible") && !Bool(n, "visible", true)) return;
                const size_t first = Items.size();
                const std::string& t = n.Type;
                bool tooltip = true;
                if (t == "panel") {
                    static const std::unordered_map<std::string, glm::vec2> anchors = {
                        { "center", { 0.5f, 0.5f } }, { "top", { 0.5f, 0.0f } }, { "bottom", { 0.5f, 1.0f } },
                        { "left", { 0.0f, 0.5f } }, { "right", { 1.0f, 0.5f } }, { "topleft", { 0.0f, 0.0f } },
                        { "topright", { 1.0f, 0.0f } }, { "bottomleft", { 0.0f, 1.0f } }, { "bottomright", { 1.0f, 1.0f } } };
                    UIPanelOptions o;
                    o.X = Num(n, "x", 0.5f);
                    o.Y = Num(n, "y", 0.5f);
                    o.W = Num(n, "w", 400.0f);
                    o.H = Num(n, "h", 0.0f);
                    auto a = anchors.find(Raw(n, "anchor", "center"));
                    o.Pivot = a != anchors.end() ? a->second : glm::vec2(0.5f);
                    o.Title = Text(n, "title");
                    o.Close = Bool(n, "close", false);
                    o.Padding = Num(n, "padding", -1.0f);
                    o.HasColor = Color(n, "color", o.Color);
                    o.Alpha = Num(n, "alpha", -1.0f);
                    o.Image = Text(n, "image");
                    o.Slice = Num(n, "slice", -1.0f);
                    const std::string id = Prefix + "/" + Key(n) + (index ? "#" + std::to_string(index) : std::string());
                    const bool open = UI.BeginPanel(id, o);
                    Nodes(n.Children, depth + 1);
                    UI.EndPanel();
                    if (!open) {
                        Result.Clicked = Key(n);
                        Result.Index = index;
                        const std::string code = Raw(n, "on_close");
                        if (!code.empty()) Eval.Run(code);
                    }
                    tooltip = false;
                }
                else if (t == "row") {
                    UI.BeginRow(Num(n, "spacing", -1.0f));
                    Nodes(n.Children, depth + 1);
                    UI.EndRow();
                }
                else if (t == "column") {
                    UI.BeginColumn(Num(n, "width", 0.0f));
                    Nodes(n.Children, depth + 1);
                    UI.EndColumn();
                }
                else if (t == "scroll") {
                    UI.BeginScroll(Key(n), Num(n, "height", 200.0f));
                    Nodes(n.Children, depth + 1);
                    UI.EndScroll();
                    tooltip = false;
                }
                else if (t == "label") {
                    UITextOptions o;
                    o.Size = Num(n, "size", 0.0f);
                    o.HasColor = Color(n, "color", o.Color);
                    o.Alpha = Num(n, "alpha", 1.0f);
                    const std::string align = Raw(n, "align", "left");
                    o.Align = align == "center" ? 1 : align == "right" ? 2 : 0;
                    o.Bold = Bool(n, "bold", false);
                    o.Muted = Bool(n, "muted", false);
                    o.Wrap = Bool(n, "wrap", true);
                    o.Shadow = Bool(n, "shadow", false);
                    o.Width = Num(n, "width", 0.0f);
                    UI.Label(Text(n, "text"), o);
                }
                else if (t == "button") {
                    UIButtonOptions o;
                    o.Id = Key(n);
                    o.W = Num(n, "w", 0.0f);
                    o.H = Num(n, "h", 0.0f);
                    o.Fill = Bool(n, "fill", false);
                    o.Size = Num(n, "size", 0.0f);
                    o.HasColor = Color(n, "color", o.Color);
                    o.Disabled = Bool(n, "disabled", false);
                    o.Selected = Bool(n, "selected", false);
                    o.Tooltip = Text(n, "tooltip");
                    tooltip = false;                 // the button shows it itself
                    if (UI.Button(Text(n, "text"), o)) Clicked(n, index);
                }
                else if (t == "image") {
                    glm::vec3 c(1.0f);
                    Color(n, "color", c);
                    UI.Image(Text(n, "path"), Num(n, "w", 64.0f), Num(n, "h", 0.0f), c, Num(n, "alpha", 1.0f));
                }
                else if (t == "bar") {
                    glm::vec3 c(0.3f, 0.85f, 0.4f), back(0.1f);
                    Color(n, "color", c);
                    Color(n, "back", back);
                    UI.Bar(Num(n, "value", 0.0f), Num(n, "w", 0.0f), Num(n, "h", 0.0f), c, back, Text(n, "text"));
                }
                else if (t == "slot") {
                    UISlotOptions o;
                    o.Icon = Text(n, "icon");
                    o.Count = (int)Num(n, "count", 0.0f);
                    o.Size = Num(n, "size", 0.0f);
                    o.Tooltip = Text(n, "tooltip");
                    o.Highlight = Bool(n, "highlight", false);
                    o.HasColor = Color(n, "color", o.Color);
                    tooltip = false;
                    const std::string id = Key(n) + (index ? "#" + std::to_string(index) : std::string());
                    if (UI.Slot(id, o).Clicked) Clicked(n, index);
                }
                else if (t == "space") { UI.Space(Num(n, "h", UI.Theme.Spacing)); tooltip = false; }
                else if (t == "separator") { UI.Separator(); tooltip = false; }
                else return;

                if (tooltip) {
                    const std::string tip = Text(n, "tooltip");
                    if (!tip.empty()) UI.Tooltip(tip);
                }
                if (Rects) {
                    UINodeRect r;
                    r.Id = n.Id;
                    r.Depth = depth;
                    UI.LastRect(r.X, r.Y, r.W, r.H);
                    if (first < Items.size()) { r.First = first; r.FirstY = Items[first].Y; }
                    Rects->push_back(r);
                }
            }
        };
    }

    UIDocResult DrawUIDocument(UIContext& ui, const UIDocument& doc, UIDocEvaluator& eval, const std::string& idPrefix,
        const std::vector<ScriptUIItem>& items, std::vector<UINodeRect>* rects) {
        Walker w{ ui, eval, idPrefix, items, rects, {} };
        w.Nodes(doc.Widgets, 0);
        return w.Result;
    }

    void FinishUIRects(const std::vector<ScriptUIItem>& items, std::vector<UINodeRect>& rects) {
        for (UINodeRect& r : rects) {
            if (r.First < items.size()) r.Y += items[r.First].Y - r.FirstY;   // moved down by a row since
            r.First = SIZE_MAX;
        }
    }

} // namespace Engine
