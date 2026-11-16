#include "Renderer/ShaderGraph.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;

namespace Engine {

    // ================================================================ file
    namespace {
        glm::vec4 Vec(const json& j, glm::vec4 def = glm::vec4(0.0f)) {
            if (j.is_number()) return glm::vec4(j.get<float>(), def.y, def.z, def.w);
            if (!j.is_array()) return def;
            for (size_t i = 0; i < j.size() && i < 4; i++) if (j[i].is_number()) def[(int)i] = j[i].get<float>();
            return def;
        }
        int Dims(const std::string& type) {
            return type == "float" ? 1 : type == "vec2" ? 2 : (type == "vec3" || type == "color") ? 3 : type == "vec4" ? 4 : type == "texture" ? 5 : 1;
        }
    }

    json ShaderGraph::ToJson() const {
        json j = json::object();
        j["version"] = 1;
        j["settings"] = { { "unlit", Unlit } };
        json ps = json::array();
        for (const SGParam& p : Params) {
            json jp = { { "name", p.Name }, { "type", p.Type } };
            if (p.Type == "texture") { jp["default"] = p.Texture; if (p.Linear) jp["linear"] = true; }
            else {
                const int n = p.Type == "color" ? 3 : Dims(p.Type);
                json d = json::array();
                for (int i = 0; i < n; i++) d.push_back(p.Default[i]);
                jp["default"] = n == 1 ? d[0] : d;
            }
            ps.push_back(jp);
        }
        j["params"] = ps;
        json ns = json::array();
        for (const SGNode& n : Nodes)
            ns.push_back({ { "id", n.Id }, { "type", n.Type }, { "pos", json::array({ n.Pos.x, n.Pos.y }) }, { "values", n.Values } });
        j["nodes"] = ns;
        json ls = json::array();
        for (const SGLink& l : Links)
            ls.push_back({ { "from", json::array({ l.FromNode, l.FromPin }) }, { "to", json::array({ l.ToNode, l.ToPin }) } });
        j["links"] = ls;
        return j;
    }

    ShaderGraph ShaderGraph::FromJson(const json& j) {
        ShaderGraph g;
        if (!j.is_object()) return g;
        if (auto s = j.find("settings"); s != j.end() && s->is_object()) g.Unlit = s->value("unlit", false);
        if (auto ps = j.find("params"); ps != j.end() && ps->is_array())
            for (const json& jp : *ps) {
                if (!jp.is_object() || !jp.contains("name")) continue;
                SGParam p;
                p.Name = jp.value("name", std::string());
                p.Type = jp.value("type", std::string("float"));
                if (p.Type == "texture") { p.Texture = jp.value("default", std::string()); p.Linear = jp.value("linear", false); }
                else if (jp.contains("default")) p.Default = Vec(jp["default"], p.Type == "color" ? glm::vec4(0, 0, 0, 1) : glm::vec4(0.0f));
                if (!p.Name.empty()) g.Params.push_back(p);
            }
        if (auto ns = j.find("nodes"); ns != j.end() && ns->is_array())
            for (const json& jn : *ns) {
                if (!jn.is_object()) continue;
                SGNode n;
                n.Id = jn.value("id", 0);
                n.Type = jn.value("type", std::string());
                if (jn.contains("pos")) { const glm::vec4 p = Vec(jn["pos"]); n.Pos = glm::vec2(p); }
                if (jn.contains("values") && jn["values"].is_object()) n.Values = jn["values"];
                if (n.Id > 0 && !n.Type.empty()) g.Nodes.push_back(n);
            }
        if (auto ls = j.find("links"); ls != j.end() && ls->is_array())
            for (const json& jl : *ls) {
                if (!jl.is_object() || !jl.contains("from") || !jl.contains("to")) continue;
                const json& f = jl["from"]; const json& t = jl["to"];
                if (!f.is_array() || f.size() != 2 || !t.is_array() || t.size() != 2) continue;
                SGLink l;
                l.FromNode = f[0].get<int>(); l.FromPin = f[1].get<std::string>();
                l.ToNode = t[0].get<int>(); l.ToPin = t[1].get<std::string>();
                g.Links.push_back(l);
            }
        return g;
    }

    ShaderGraph ShaderGraph::Default() {
        ShaderGraph g;
        SGParam tint; tint.Name = "Color"; tint.Type = "color"; tint.Default = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
        g.Params.push_back(tint);
        SGNode out; out.Id = 1; out.Type = "Output"; out.Pos = glm::vec2(320.0f, 0.0f);
        SGNode prop; prop.Id = 2; prop.Type = "Property"; prop.Pos = glm::vec2(0.0f, 0.0f); prop.Values["param"] = "Color";
        g.Nodes = { out, prop };
        g.Links.push_back({ 2, "Value", 1, "BaseColor" });
        return g;
    }

    const SGNode* ShaderGraph::Find(int id) const { for (const SGNode& n : Nodes) if (n.Id == id) return &n; return nullptr; }
    SGNode* ShaderGraph::Find(int id) { for (SGNode& n : Nodes) if (n.Id == id) return &n; return nullptr; }
    int ShaderGraph::NextId() const { int m = 0; for (const SGNode& n : Nodes) m = std::max(m, n.Id); return m + 1; }
    const SGParam* ShaderGraph::Param(const std::string& name) const { for (const SGParam& p : Params) if (p.Name == name) return &p; return nullptr; }

    // ================================================================ node library
    namespace {
        SGPinDef P(const std::string& name, int type, glm::vec4 def = glm::vec4(0.0f), const std::string& expr = "") { return { name, type, def, expr }; }
        const glm::vec4 One(1.0f), Half(0.5f), Up(0.0f, 0.0f, 1.0f, 0.0f);

        void Add(std::vector<SGNodeDef>& v, const std::string& type, const std::string& cat, const std::string& help,
                 std::vector<SGPinDef> in, std::vector<std::pair<SGPinDef, std::string>> out, bool special = false) {
            SGNodeDef d;
            d.Type = type; d.Category = cat; d.Help = help; d.In = std::move(in); d.Special = special;
            for (auto& [p, e] : out) { d.Out.push_back(p); d.OutExpr.push_back(e); }
            v.push_back(std::move(d));
        }

        std::vector<SGNodeDef> BuildNodes() {
            std::vector<SGNodeDef> v;
            const std::string uv = "IN.UV";
            // Output
            Add(v, "Output", "Output", "The surface: what the material looks like. Unconnected inputs use their values. Vertex Offset moves the mesh's points (world space, metres): waves, wobble, wind.",
                { P("BaseColor", 3, glm::vec4(0.8f)), P("Alpha", 1, One), P("Metallic", 1), P("Roughness", 1, Half),
                  P("Specular", 1, One), P("Normal", 3, Up), P("Emission", 3), P("Vertex Offset", 3) }, {}, true);
            // Input
            Add(v, "UV", "Input", "The mesh's texture coordinates (the material's UV scale and offset applied).", {}, { { P("UV", 2), "IN.UV" } });
            Add(v, "Position", "Input", "The point being drawn, in world space (metres).", {}, { { P("Position", 3), "IN.WorldPos" } });
            Add(v, "Normal", "Input", "The surface's normal in world space (before any normal map).", {}, { { P("Normal", 3), "IN.WorldNormal" } });
            Add(v, "View Direction", "Input", "From the surface towards the camera, world space, length 1.", {}, { { P("Direction", 3), "IN.ViewDir" } });
            Add(v, "Screen Position", "Input", "Where the pixel is on the screen, 0..1.", {}, { { P("UV", 2), "IN.ScreenUV" } });
            Add(v, "Time", "Input", "Seconds (the scene clock: it runs in the editor too).", {},
                { { P("Time", 1), "IN.Time" }, { P("Sin", 1), "sin(IN.Time)" }, { P("Cos", 1), "cos(IN.Time)" } });
            Add(v, "Float", "Input", "A number.", {}, { { P("Value", 1), "" } }, true);
            Add(v, "Vector2", "Input", "Two numbers.", {}, { { P("Value", 2), "" } }, true);
            Add(v, "Vector3", "Input", "Three numbers.", {}, { { P("Value", 3), "" } }, true);
            Add(v, "Vector4", "Input", "Four numbers.", {}, { { P("Value", 4), "" } }, true);
            Add(v, "Color", "Input", "A colour (picked in sRGB, used in linear light).", {}, { { P("RGB", 3), "" }, { P("A", 1), "" } }, true);
            Add(v, "Property", "Input", "A param of the graph: materials using the graph set its value.", {}, { { P("Value", 1), "" } }, true);
            // Texture
            Add(v, "Sample Texture", "Texture", "Reads a texture (a texture property, or its own image) at UV.",
                { P("Texture", 5), P("UV", 2, glm::vec4(0.0f), uv) },
                { { P("RGBA", 4), "" }, { P("RGB", 3), "" }, { P("R", 1), "" }, { P("G", 1), "" }, { P("B", 1), "" }, { P("A", 1), "" } }, true);
            Add(v, "Sample Normal Map", "Texture", "Reads a tangent-space normal map (OpenGL style; use a linear texture).",
                { P("Texture", 5), P("UV", 2, glm::vec4(0.0f), uv), P("Strength", 1, One) }, { { P("Normal", 3), "" } }, true);
            Add(v, "Triplanar", "Texture", "Projects a texture from the three world axes: no UVs needed (rocks, cliffs).",
                { P("Texture", 5), P("Scale", 1, Half), P("Sharpness", 1, glm::vec4(4.0f)) }, { { P("RGBA", 4), "" }, { P("RGB", 3), "" } }, true);
            Add(v, "Tiling And Offset", "UV", "UV x Tiling + Offset.", { P("UV", 2, glm::vec4(0.0f), uv), P("Tiling", 2, One), P("Offset", 2) },
                { { P("UV", 2), "($0 * $1 + $2)" } });
            Add(v, "Panner", "UV", "Slides UVs over time (flowing water, scrolling lava).", { P("UV", 2, glm::vec4(0.0f), uv), P("Speed", 2, glm::vec4(0.1f, 0.0f, 0.0f, 0.0f)) },
                { { P("UV", 2), "($0 + $1 * IN.Time)" } });
            Add(v, "Rotate UV", "UV", "Turns UVs round a centre (radians).", { P("UV", 2, glm::vec4(0.0f), uv), P("Center", 2, Half), P("Angle", 1) },
                { { P("UV", 2), "sg_rotate($0, $1, $2)" } });
            Add(v, "World UV", "UV", "World X / Z as UVs, in repeats per metre: textures that line up across objects.",
                { P("Scale", 1, Half) }, { { P("UV", 2), "(IN.WorldPos.xz * $0)" } });
            // Math (dynamic)
            auto un = [&](const std::string& t, const std::string& e, const std::string& h) { Add(v, t, "Math", h, { P("In", 0) }, { { P("Out", 0), e } }); };
            auto bin = [&](const std::string& t, const std::string& e, const std::string& h, glm::vec4 b = glm::vec4(0.0f)) {
                Add(v, t, "Math", h, { P("A", 0), P("B", 0, b) }, { { P("Out", 0), e } }); };
            bin("Add", "($0 + $1)", "A + B");
            bin("Subtract", "($0 - $1)", "A - B");
            bin("Multiply", "($0 * $1)", "A x B", One);
            bin("Divide", "($0 / $1)", "A / B", One);
            bin("Power", "pow(abs($0), $1)", "A to the power B", One);
            bin("Minimum", "min($0, $1)", "The smaller of A and B");
            bin("Maximum", "max($0, $1)", "The larger of A and B");
            bin("Modulo", "mod($0, $1)", "The remainder of A / B", One);
            bin("Step", "step($0, $1)", "0 below the edge A, 1 from it on (B is the input)");
            un("One Minus", "(1.0 - $0)", "1 - In");
            un("Negate", "(-$0)", "-In");
            un("Absolute", "abs($0)", "In without its sign");
            un("Floor", "floor($0)", "Rounded down");
            un("Ceiling", "ceil($0)", "Rounded up");
            un("Fraction", "fract($0)", "The part after the point");
            un("Square Root", "sqrt(abs($0))", "The square root");
            un("Sine", "sin($0)", "sin(In)");
            un("Cosine", "cos($0)", "cos(In)");
            un("Saturate", "clamp($0, 0.0, 1.0)", "Clamped to 0..1");
            un("Normalize", "normalize($0)", "The same direction, length 1");
            Add(v, "Clamp", "Math", "In kept between Min and Max.", { P("In", 0), P("Min", 0), P("Max", 0, One) }, { { P("Out", 0), "clamp($0, $1, $2)" } });
            Add(v, "Lerp", "Math", "A blended towards B by T (0 = A, 1 = B).", { P("A", 0), P("B", 0, One), P("T", 0, Half) }, { { P("Out", 0), "mix($0, $1, $2)" } });
            Add(v, "Smoothstep", "Math", "A smooth 0..1 ramp of In from Edge 1 to Edge 2.", { P("Edge 1", 0), P("Edge 2", 0, One), P("In", 0, Half) },
                { { P("Out", 0), "smoothstep($0, $1, $2)" } });
            Add(v, "Remap", "Math", "In from the In range to the Out range.", { P("In", 0), P("In Min", 0), P("In Max", 0, One), P("Out Min", 0), P("Out Max", 0, One) },
                { { P("Out", 0), "($3 + ($0 - $1) * ($4 - $3) / ($2 - $1))" } });
            Add(v, "Posterize", "Math", "In in Steps flat bands.", { P("In", 0), P("Steps", 0, glm::vec4(4.0f)) }, { { P("Out", 0), "(floor($0 * $1) / $1)" } });
            Add(v, "Length", "Math", "The length of a vector.", { P("In", 0) }, { { P("Out", 1), "length($0)" } });
            Add(v, "Distance", "Math", "The distance between A and B.", { P("A", 0), P("B", 0) }, { { P("Out", 1), "distance($0, $1)" } });
            Add(v, "Dot Product", "Math", "dot(A, B): 1 when they point the same way, 0 at right angles.", { P("A", 3), P("B", 3, Up) }, { { P("Out", 1), "dot($0, $1)" } });
            Add(v, "Cross Product", "Math", "A vector at right angles to A and B.", { P("A", 3), P("B", 3, Up) }, { { P("Out", 3), "cross($0, $1)" } });
            // Vector
            Add(v, "Split", "Vector", "The components of a vector.", { P("In", 4) },
                { { P("R", 1), "$0.x" }, { P("G", 1), "$0.y" }, { P("B", 1), "$0.z" }, { P("A", 1), "$0.w" } });
            Add(v, "Combine", "Vector", "Builds a vector from numbers.", { P("R", 1), P("G", 1), P("B", 1), P("A", 1, One) },
                { { P("RGBA", 4), "vec4($0, $1, $2, $3)" }, { P("RGB", 3), "vec3($0, $1, $2)" }, { P("RG", 2), "vec2($0, $1)" } });
            // Procedural
            Add(v, "Noise", "Procedural", "Smooth value noise, 0..1.", { P("UV", 2, glm::vec4(0.0f), uv), P("Scale", 1, glm::vec4(10.0f)) },
                { { P("Out", 1), "sg_noise($0 * $1)" } });
            Add(v, "Fractal Noise", "Procedural", "Five layers of noise, finer and fainter: clouds, dirt, rock.", { P("UV", 2, glm::vec4(0.0f), uv), P("Scale", 1, glm::vec4(4.0f)) },
                { { P("Out", 1), "sg_fbm($0 * $1)" } });
            Add(v, "Voronoi", "Procedural", "Cells: Distance to the nearest point (0 at it), Cell a random value per cell.",
                { P("UV", 2, glm::vec4(0.0f), uv), P("Scale", 1, glm::vec4(5.0f)) },
                { { P("Distance", 1), "sg_voronoi($0 * $1).x" }, { P("Cell", 1), "sg_voronoi($0 * $1).y" } });
            Add(v, "Checker", "Procedural", "A checkerboard of two colours.", { P("UV", 2, glm::vec4(0.0f), uv), P("Scale", 2, glm::vec4(8.0f)),
                P("A", 3, glm::vec4(0.15f)), P("B", 3, glm::vec4(0.85f)) }, { { P("Out", 3), "mix($2, $3, sg_checker($0 * $1))" } });
            Add(v, "Waves", "Procedural", "Four sine waves rolling along Direction (world X / Z), at deep-water speeds: Height for "
                "Vertex Offset (as its Y), Normal (tangent space, for the plane / grid's UVs, +X and -Z) and Slope (steep crests: foam).",
                { P("Position", 3, glm::vec4(0.0f), "IN.WorldPos"), P("Direction", 2, glm::vec4(1.0f, 0.3f, 0.0f, 0.0f)),
                  P("Wavelength", 1, glm::vec4(6.0f)), P("Height", 1, glm::vec4(0.15f)), P("Speed", 1, One) },
                { { P("Height", 1), "sg_waves($0.xz, $1, $2, $3, $4, IN.Time).x" },
                  { P("Normal", 3), "sg_wavesNormal(sg_waves($0.xz, $1, $2, $3, $4, IN.Time))" },
                  { P("Slope", 1), "length(sg_waves($0.xz, $1, $2, $3, $4, IN.Time).yz)" } });
            Add(v, "Gradient", "Procedural", "A ramp along UV: 0 at the start, 1 at the end (Angle in radians).",
                { P("UV", 2, glm::vec4(0.0f), uv), P("Angle", 1) }, { { P("Out", 1), "clamp(dot($0 - 0.5, vec2(cos($1), sin($1))) + 0.5, 0.0, 1.0)" } });
            // Surface
            Add(v, "Fresnel", "Surface", "1 at the edges seen side-on, 0 facing the camera (rim light, ghosts, soap).",
                { P("Power", 1, glm::vec4(5.0f)) }, { { P("Out", 1), "pow(1.0 - clamp(dot(normalize(IN.WorldNormal), IN.ViewDir), 0.0, 1.0), $0)" } });
            Add(v, "Desaturate", "Surface", "Towards grey by Amount.", { P("Color", 3, One), P("Amount", 1, One) },
                { { P("Out", 3), "mix($0, vec3(dot($0, vec3(0.2126, 0.7152, 0.0722))), $1)" } });
            Add(v, "Contrast", "Surface", "Spreads colours away from (or towards) mid grey.", { P("Color", 3, Half), P("Contrast", 1, One) },
                { { P("Out", 3), "max(($0 - 0.18) * $1 + 0.18, 0.0)" } });
            Add(v, "Hue Shift", "Surface", "Turns the hue (0..1 = a full turn).", { P("Color", 3, One), P("Shift", 1) }, { { P("Out", 3), "sg_hue($0, $1)" } });
            Add(v, "Normal Blend", "Surface", "Two tangent normals together (a detail map over a base one).", { P("A", 3, Up), P("B", 3, Up) },
                { { P("Out", 3), "sg_normalBlend($0, $1)" } });
            Add(v, "Normal Strength", "Surface", "Flattens (below 1) or deepens a tangent normal.", { P("In", 3, Up), P("Strength", 1, One) },
                { { P("Out", 3), "normalize(vec3($0.xy * $1, $0.z))" } });
            Add(v, "Height Blend", "Surface", "Blends B over A where B's height (plus Offset) is above A's: stones through moss.",
                { P("A", 3), P("B", 3, One), P("Height A", 1, Half), P("Height B", 1, Half), P("Offset", 1), P("Softness", 1, glm::vec4(0.1f)) },
                { { P("Out", 3), "mix($0, $1, smoothstep(-$5, $5, ($3 + $4) - $2))" } });
            // Logic
            Add(v, "Compare", "Logic", "True when A > B, else False.", { P("A", 1), P("B", 1), P("True", 0, One), P("False", 0) },
                { { P("Out", 0), "(($0 > $1) ? $2 : $3)" } });
            return v;
        }
    }

    const std::vector<SGNodeDef>& ShaderGraphNodes() { static const std::vector<SGNodeDef> v = BuildNodes(); return v; }
    const SGNodeDef* ShaderGraphNode(const std::string& type) {
        for (const SGNodeDef& d : ShaderGraphNodes()) if (d.Type == type) return &d;
        return nullptr;
    }
    const char* ShaderGraphTypeName(int t) {
        switch (t) { case 1: return "float"; case 2: return "vec2"; case 3: return "vec3"; case 4: return "vec4"; case 5: return "texture"; default: return "any"; }
    }
    std::vector<SGPinDef> ShaderGraphOutputs(const ShaderGraph& g, const SGNode& n) {
        const SGNodeDef* d = ShaderGraphNode(n.Type);
        if (!d) return {};
        if (n.Type == "Property") {
            const SGParam* p = g.Param(n.Values.value("param", std::string()));
            const int t = p ? Dims(p->Type) : 1;
            return { P(t == 5 ? "Texture" : "Value", t) };
        }
        return d->Out;
    }

    // ================================================================ compiler
    namespace {
        const char* kHelpers = R"(
float sg_hash(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float sg_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    return mix(mix(sg_hash(i), sg_hash(i + vec2(1, 0)), u.x), mix(sg_hash(i + vec2(0, 1)), sg_hash(i + vec2(1, 1)), u.x), u.y);
}
float sg_fbm(vec2 p) { float s = 0.0, a = 0.5; for (int k = 0; k < 5; k++) { s += a * sg_noise(p); p = p * 2.03 + 17.1; a *= 0.5; } return s / 0.96875; }
vec2 sg_voronoi(vec2 p) {
    vec2 i = floor(p), f = fract(p); float d = 8.0, cell = 0.0;
    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
        vec2 g = vec2(x, y), o = vec2(sg_hash(i + g), sg_hash(i + g + 31.7));
        float dd = length(g + o - f);
        if (dd < d) { d = dd; cell = sg_hash(i + g + 11.3); }
    }
    return vec2(d, cell);
}
float sg_checker(vec2 p) { vec2 c = floor(p); return mod(c.x + c.y, 2.0); }
vec2 sg_rotate(vec2 uv, vec2 c, float a) { float s = sin(a), k = cos(a); uv -= c; return vec2(k * uv.x - s * uv.y, s * uv.x + k * uv.y) + c; }
vec3 sg_unpackNormal(vec4 t, float s) { vec3 n = t.xyz * 2.0 - 1.0; n.xy *= s; return normalize(n); }
vec3 sg_normalBlend(vec3 a, vec3 b) { return normalize(vec3(a.xy + b.xy, a.z * b.z)); }
vec3 sg_hue(vec3 c, float h) {
    const vec3 k = vec3(0.57735); float a = h * 6.28318;
    return c * cos(a) + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - cos(a));
}
// Four directional sine waves (angles and lengths spread about the main one), deep-water dispersion: omega = sqrt(g k).
// x = height, y / z = its slope along world X / Z
vec3 sg_waves(vec2 p, vec2 dir, float len, float amp, float speed, float t) {
    const float turn[4] = float[](0.0, 0.45, -0.6, 1.1);
    const float scale[4] = float[](1.0, 0.62, 0.41, 0.27);
    float a0 = atan(dir.y, dir.x + 1e-6);
    vec3 r = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        float k = 6.28318 / max(len * scale[i], 0.01);
        vec2 d = vec2(cos(a0 + turn[i]), sin(a0 + turn[i]));
        float ph = k * dot(d, p) - sqrt(9.81 * k) * speed * t + float(i) * 1.7;
        float a = amp * scale[i];
        r.x += a * sin(ph);
        r.yz += d * (a * k * cos(ph));
    }
    return r;
}
// Its normal in the tangent space of a plane / grid (u along +X, v along -Z)
vec3 sg_wavesNormal(vec3 w) { return normalize(vec3(-w.y, w.z, 1.0)); }
vec4 sg_triplanar(sampler2D t, vec3 p, vec3 n, float scale, float sharp) {
    vec3 w = pow(abs(normalize(n)), vec3(sharp)); w /= (w.x + w.y + w.z + 1e-5);
    return texture(t, p.zy * scale) * w.x + texture(t, p.xz * scale) * w.y + texture(t, p.xy * scale) * w.z;
}
)";

        std::string F(float f) {
            if (!std::isfinite(f)) f = 0.0f;
            char b[32];
            std::snprintf(b, sizeof(b), "%.6g", f);
            std::string s = b;
            if (s.find_first_of(".e") == std::string::npos) s += ".0";
            return s;
        }
        std::string TypeName(int t) { return t == 1 ? "float" : t == 2 ? "vec2" : t == 3 ? "vec3" : "vec4"; }
        std::string Literal(const glm::vec4& v, int t) {
            if (t == 1) return F(v.x);
            std::string s = TypeName(t) + "(";
            for (int i = 0; i < t; i++) s += (i ? ", " : "") + F(v[i]);
            return s + ")";
        }
        std::string Conv(const std::string& e, int from, int to) {
            if (from == to || to == 0) return e;
            if (from == 1) return TypeName(to) + "(" + e + ")";
            if (from > to) return "(" + e + ")" + (to == 1 ? ".x" : to == 2 ? ".xy" : ".xyz");
            if (from == 2 && to == 3) return "vec3(" + e + ", 0.0)";
            if (from == 2 && to == 4) return "vec4(" + e + ", 0.0, 1.0)";
            return "vec4(" + e + ", 1.0)";                            // 3 -> 4
        }
        float SrgbToLinear(float c) { return std::pow(std::max(c, 0.0f), 2.2f); }

        struct Compiler {
            const ShaderGraph& G;
            SGCompiled R;
            std::ostringstream Body;
            std::map<int, std::vector<std::pair<std::string, int>>> Done;   // node -> its outputs (variable, type)
            std::set<int> Visiting;
            std::map<std::string, std::string> ParamUniform;                   // param name -> uniform
            std::map<int, std::string> HiddenTex;                              // a sample node's own image -> its uniform
            int NextUnit = 18;                                                  // 0..17 are lit.frag's

            explicit Compiler(const ShaderGraph& g) : G(g) {}

            void Error(int node, const std::string& msg) {
                if (node) { if (!R.NodeErrors.count(node)) R.NodeErrors[node] = msg; }
                else R.Errors.push_back(msg);
            }
            const SGLink* LinkInto(int node, const std::string& pin) const {
                for (const SGLink& l : G.Links) if (l.ToNode == node && l.ToPin == pin) return &l;
                return nullptr;
            }
            std::string AddParam(const SGParam& p) {
                const std::string u = "u_SG" + std::to_string(R.Params.size());
                R.Params.push_back(p);
                R.Uniforms.push_back(u);
                int unit = -1;
                if (p.Type == "texture") {
                    if (NextUnit > 31) { Error(0, "too many textures (at most 14)"); unit = 31; }
                    else unit = NextUnit++;
                }
                R.TextureUnits.push_back(unit);
                return u;
            }

            // An input of node n: (GLSL expression, type), linked or its literal / default
            std::pair<std::string, int> Input(const SGNode& n, const SGPinDef& pin) {
                if (const SGLink* l = LinkInto(n.Id, pin.Name)) {
                    if (!Eval(l->FromNode)) return { pin.Type == 5 ? "" : Literal(pin.Default, std::max(pin.Type, 1)), pin.Type == 5 ? 5 : std::max(pin.Type, 1) };
                    const SGNode* src = G.Find(l->FromNode);
                    const auto outs = ShaderGraphOutputs(G, *src);
                    for (size_t k = 0; k < outs.size(); k++)
                        if (outs[k].Name == l->FromPin && k < Done[l->FromNode].size()) return Done[l->FromNode][k];
                    Error(n.Id, "the link into " + pin.Name + " comes from a pin that no longer exists");
                }
                if (pin.Type == 5) return { "", 5 };
                if (!pin.Expr.empty() && !n.Values.contains(pin.Name)) return { pin.Expr, pin.Type };
                glm::vec4 v = pin.Default;
                int t = std::max(pin.Type, 1);
                if (auto it = n.Values.find(pin.Name); it != n.Values.end()) {
                    v = Vec(*it, v);
                    if (pin.Type == 0) t = it->is_array() ? std::clamp((int)it->size(), 1, 4) : 1;
                }
                return { Literal(v, t), t };
            }

            bool Eval(int id) {
                if (Done.count(id)) return true;
                const SGNode* n = G.Find(id);
                if (!n) { Error(0, "a link leads to node " + std::to_string(id) + ", which is gone"); return false; }
                const SGNodeDef* d = ShaderGraphNode(n->Type);
                if (!d) { Error(id, "unknown node type " + n->Type); return false; }
                if (Visiting.count(id)) { Error(id, "a loop: this node feeds itself"); return false; }
                Visiting.insert(id);
                const bool ok = d->Special ? EvalSpecial(*n, *d) : EvalTemplate(*n, *d);
                Visiting.erase(id);
                return ok;
            }

            bool EvalTemplate(const SGNode& n, const SGNodeDef& d) {
                std::vector<std::pair<std::string, int>> in;
                int dyn = 1;
                for (const SGPinDef& p : d.In) {
                    in.push_back(Input(n, p));
                    if (in.back().second == 5) { Error(n.Id, p.Name + " can't take a texture"); return false; }
                    if (p.Type == 0) dyn = std::max(dyn, in.back().second);
                }
                std::vector<std::string> args;
                for (size_t i = 0; i < d.In.size(); i++)
                    args.push_back(Conv(in[i].first, in[i].second, d.In[i].Type == 0 ? dyn : d.In[i].Type));
                auto& outs = Done[n.Id];
                for (size_t k = 0; k < d.Out.size(); k++) {
                    std::string e = d.OutExpr[k];
                    for (int i = (int)args.size() - 1; i >= 0; i--) {            // $10 before $1
                        const std::string key = "$" + std::to_string(i);
                        for (size_t pos = e.find(key); pos != std::string::npos; pos = e.find(key, pos + args[i].size()))
                            e.replace(pos, key.size(), "(" + args[i] + ")");
                    }
                    const int t = d.Out[k].Type == 0 ? dyn : d.Out[k].Type;
                    const std::string var = "n" + std::to_string(n.Id) + "_" + std::to_string(k);
                    Body << "    " << TypeName(t) << " " << var << " = " << e << ";\n";
                    outs.push_back({ var, t });
                }
                return true;
            }

            // A texture input: a linked texture property, else the node's own image (a hidden param)
            std::string TextureInput(const SGNode& n, const SGPinDef& pin, bool linear) {
                auto in = Input(n, pin);
                if (LinkInto(n.Id, pin.Name)) {
                    if (in.second != 5) { Error(n.Id, pin.Name + " needs a texture property, not a " + std::string(ShaderGraphTypeName(in.second))); return ""; }
                    return in.first;
                }
                const std::string path = n.Values.value("texture", std::string());
                if (path.empty()) { Error(n.Id, "no texture: connect a texture property or pick an image"); return ""; }
                if (auto it = HiddenTex.find(n.Id); it != HiddenTex.end()) return it->second;   // already made (the other function)
                SGParam p;
                p.Name = "_node" + std::to_string(n.Id);
                p.Type = "texture";
                p.Texture = path;
                p.Linear = linear || n.Values.value("linear", false);
                return HiddenTex[n.Id] = AddParam(p);
            }

            bool EvalSpecial(const SGNode& n, const SGNodeDef& d) {
                auto& outs = Done[n.Id];
                const std::string pre = "n" + std::to_string(n.Id);
                auto out = [&](const std::string& expr, int t) {
                    const std::string var = pre + "_" + std::to_string(outs.size());
                    Body << "    " << TypeName(t) << " " << var << " = " << expr << ";\n";
                    outs.push_back({ var, t });
                };
                const std::string& t = n.Type;
                if (t == "Float" || t == "Vector2" || t == "Vector3" || t == "Vector4") {
                    const int dims = t == "Float" ? 1 : t[6] - '0';
                    out(Literal(Vec(n.Values.value("Value", json::array()), glm::vec4(0.0f)), dims), dims);
                    return true;
                }
                if (t == "Color") {
                    glm::vec4 c = Vec(n.Values.value("Color", json::array()), glm::vec4(1.0f));
                    out(Literal(glm::vec4(SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b), 0.0f), 3), 3);
                    out(F(c.a), 1);
                    return true;
                }
                if (t == "Property") {
                    const std::string name = n.Values.value("param", std::string());
                    const SGParam* p = G.Param(name);
                    if (!p) { Error(n.Id, name.empty() ? "pick a property" : "no property called " + name); out("0.0", 1); return false; }
                    const std::string u = ParamUniform[p->Name];
                    if (p->Type == "texture") { outs.push_back({ u, 5 }); return true; }
                    const int dims = Dims(p->Type);
                    out(u, dims);
                    return true;
                }
                if (t == "Sample Texture" || t == "Sample Normal Map" || t == "Triplanar") {
                    const std::string tex = TextureInput(n, d.In[0], t == "Sample Normal Map");
                    if (tex.empty()) { for (size_t k = 0; k < d.Out.size(); k++) out(Literal(glm::vec4(0.0f), d.Out[k].Type), d.Out[k].Type); return false; }
                    if (t == "Triplanar") {
                        const auto sc = Input(n, d.In[1]), sh = Input(n, d.In[2]);
                        out("sg_triplanar(" + tex + ", IN.WorldPos, IN.WorldNormal, " + Conv(sc.first, sc.second, 1) + ", " + Conv(sh.first, sh.second, 1) + ")", 4);
                        out(outs[0].first + ".rgb", 3);
                        return true;
                    }
                    const auto uvIn = Input(n, d.In[1]);
                    const std::string sample = "texture(" + tex + ", " + Conv(uvIn.first, uvIn.second, 2) + ")";
                    if (t == "Sample Normal Map") {
                        const auto s = Input(n, d.In[2]);
                        out("sg_unpackNormal(" + sample + ", " + Conv(s.first, s.second, 1) + ")", 3);
                        return true;
                    }
                    out(sample, 4);
                    const std::string v = outs[0].first;
                    out(v + ".rgb", 3); out(v + ".r", 1); out(v + ".g", 1); out(v + ".b", 1); out(v + ".a", 1);
                    return true;
                }
                if (t == "Output") return true;                          // compiled by Compile itself
                Error(n.Id, "can't compile " + t);
                return false;
            }

            // The structs, every param's uniform and the helpers: the same in every stage and function
            std::string Header() const {
                std::ostringstream c;
                c << "// ---- shader graph (generated)\n"
                     "struct SGIn { vec2 UV; vec3 WorldPos; vec3 WorldNormal; vec3 ViewDir; float Time; vec2 ScreenUV; };\n"
                     "struct SGOut { vec3 BaseColor; float Alpha; float Metallic; float Roughness; float Specular; vec3 Normal; bool HasNormal; vec3 Emission; };\n";
                for (size_t i = 0; i < R.Params.size(); i++) {
                    const SGParam& p = R.Params[i];
                    if (p.Type == "texture") c << "layout(binding = " << R.TextureUnits[i] << ") uniform sampler2D " << R.Uniforms[i] << ";\n";
                    else c << "uniform " << TypeName(Dims(p.Type)) << " " << R.Uniforms[i] << ";\n";
                }
                c << kHelpers;
                return c.str();
            }
            void NewFunction() { Body.str(""); Body.clear(); Done.clear(); }   // the next function declares its own variables

            const SGNode* FindOutput() {
                const SGNode* output = nullptr;
                for (const SGNode& n : G.Nodes) if (n.Type == "Output") { if (output) Error(n.Id, "only the first Output node is used"); else output = &n; }
                if (!output) Error(0, "no Output node");
                return output;
            }

            void Compile() {
                for (const SGParam& p : G.Params) ParamUniform[p.Name] = AddParam(p);
                const SGNode* output = FindOutput();
                // The surface (fragment stage): every Output input but Vertex Offset
                std::string assign;
                if (output) {
                    const SGNodeDef& d = *ShaderGraphNode("Output");
                    for (const SGPinDef& pin : d.In) {
                        if (pin.Name == "Vertex Offset") continue;
                        const auto in = Input(*output, pin);
                        if (in.second == 5) { Error(output->Id, pin.Name + " can't take a texture"); continue; }
                        assign += "    o." + pin.Name + " = " + Conv(in.first, in.second, pin.Type) + ";\n";
                    }
                    R.WritesAlpha = LinkInto(output->Id, "Alpha") != nullptr;
                    if (LinkInto(output->Id, "Normal")) assign += "    o.HasNormal = true;\n";
                }
                R.Unlit = G.Unlit;
                const std::string surface = Body.str();

                // The vertex offset (vertex stage), only when it is connected
                std::string vertex;
                if (output && LinkInto(output->Id, "Vertex Offset")) {
                    NewFunction();
                    const SGPinDef* pin = nullptr;
                    for (const SGPinDef& p : ShaderGraphNode("Output")->In) if (p.Name == "Vertex Offset") pin = &p;
                    const auto in = Input(*output, *pin);
                    if (in.second == 5) Error(output->Id, "Vertex Offset can't take a texture");
                    else vertex = Body.str() + "    return " + Conv(in.first, in.second, 3) + ";\n";
                    R.HasVertexOffset = !vertex.empty();
                }

                const std::string head = Header();
                R.Code = head + "SGOut GraphSurface(SGIn IN) {\n" + surface +
                         "    SGOut o;\n    o.HasNormal = false;\n" + assign + "    return o;\n}\n";
                if (R.HasVertexOffset) R.VertexCode = head + "vec3 GraphVertexOffset(SGIn IN) {\n" + vertex + "}\n";
                R.Ok = R.Errors.empty() && R.NodeErrors.empty();
            }

            // One node's output alone, for its thumbnail: vec4 GraphPreview(SGIn IN)
            void CompilePreview(int nodeId, int output) {
                for (const SGParam& p : G.Params) ParamUniform[p.Name] = AddParam(p);
                std::string ret = "vec4(0.0, 0.0, 0.0, 1.0)";
                if (Eval(nodeId) && output < (int)Done[nodeId].size()) {
                    const auto& [var, type] = Done[nodeId][output];
                    if (type == 1) ret = "vec4(vec3(" + var + "), 1.0)";
                    else if (type == 2) ret = "vec4(" + var + ", 0.0, 1.0)";
                    else if (type == 3) ret = "vec4(" + var + ", 1.0)";
                    else if (type == 4) ret = "vec4(" + var + ".rgb, 1.0)";
                }
                R.Code = Header() + "vec4 GraphPreview(SGIn IN) {\n" + Body.str() + "    return " + ret + ";\n}\n";
                R.Ok = R.Errors.empty() && R.NodeErrors.empty();
                // Shown on a sphere when it depends on where or which way the surface is, else on a flat square
                std::set<int> seen;
                std::function<bool(int)> spatial = [&](int id) -> bool {
                    if (!seen.insert(id).second) return false;
                    const SGNode* n = G.Find(id);
                    if (!n) return false;
                    static const std::set<std::string> kSpatial = { "Position", "Normal", "View Direction", "Fresnel", "Triplanar", "World UV" };
                    if (kSpatial.count(n->Type)) return true;
                    for (const SGLink& l : G.Links) if (l.ToNode == id && spatial(l.FromNode)) return true;
                    return false;
                };
                R.Spatial = spatial(nodeId);
            }
        };
    }

    SGCompiled CompileShaderGraph(const ShaderGraph& g) {
        Compiler c(g);
        c.Compile();
        return std::move(c.R);
    }

    SGCompiled CompileShaderGraphPreview(const ShaderGraph& g, int nodeId, int output) {
        Compiler c(g);
        c.CompilePreview(nodeId, output);
        return std::move(c.R);
    }

    // ================================================================ programs
    namespace {
        std::string g_ShaderDir;
        std::unordered_map<std::string, std::weak_ptr<SGProgram>>& Programs() {
            static std::unordered_map<std::string, std::weak_ptr<SGProgram>> m;
            return m;
        }
    }

    SGProgram::SGProgram(std::string code, std::string vertexCode) : m_Code(std::move(code)), m_VertexCode(std::move(vertexCode)) {}
    SGProgram::~SGProgram() = default;
    void SGProgram::SetShaderDir(const std::string& dir) { g_ShaderDir = dir; }

    std::shared_ptr<SGProgram> SGProgram::For(const std::string& code, const std::string& vertexCode) {
        const std::string key = code + "\n// ---- vertex\n" + vertexCode;
        auto& m = Programs();
        if (auto it = m.find(key); it != m.end())
            if (auto p = it->second.lock()) return p;
        auto p = std::make_shared<SGProgram>(code, vertexCode);
        m[key] = p;
        return p;
    }

    void SGProgram::ReloadAll() {
        for (auto& [code, w] : Programs())
            if (auto p = w.lock()) for (int i = 0; i < 4; i++) { p->m_Variants[i].reset(); p->m_Tried[i] = false; }
    }

    Shader* SGProgram::Get(int variant) {
        if (variant < 0 || variant > 3) return nullptr;
        if (!m_Tried[variant]) {
            m_Tried[variant] = true;
            static const char* kFiles[4][2] = { { "lit.vert", "lit.frag" }, { "lit.vert", "depth_prepass.frag" },
                                                { "shadow.vert", "shadow.frag" }, { "shadow.vert", "sun_shadow.frag" } };
            Shader::Injection inj{ "#define MDB_GRAPH 1", m_Code, m_VertexCode.empty() ? "" : "#define MDB_GRAPH_VERTEX 1", m_VertexCode };
            auto s = std::make_unique<Shader>(g_ShaderDir + kFiles[variant][0], g_ShaderDir + kFiles[variant][1], std::move(inj));
            s->QuietMissing = true;               // it gets the whole lit set (Shader::ReplayInto)
            if (!s->Valid()) { m_Error = s->LastError(); ENGINE_WARN("Shader graph: {} failed to compile", kFiles[variant][1]); }
            else m_Variants[variant] = std::move(s);
        }
        return m_Variants[variant].get();
    }

    void SGBinding::Apply(Shader& s) const {
        for (const Value& v : Values) {
            switch (v.Kind) {
            case 1: s.SetFloat(v.Uniform, v.V.x); break;
            case 2: s.SetVec2(v.Uniform, glm::vec2(v.V)); break;
            case 3: s.SetVec3(v.Uniform, glm::vec3(v.V)); break;
            case 4: s.SetVec4(v.Uniform, v.V); break;
            case 5: if (v.Tex && v.Unit >= 0) v.Tex->Bind((uint32_t)v.Unit); break;
            default: break;
            }
        }
    }

} // namespace Engine

// ================================================================ node previews (the editor's thumbnails)
#include <glad/glad.h>

namespace Engine {

    struct SGNodePreviews::Entry {
        std::string Code;
        SGCompiled C;
        std::unique_ptr<Shader> Program;
        bool Dirty = true;                        // the program must be built again
        uint32_t Fbo = 0, Tex = 0;
        ~Entry() { if (Fbo) glDeleteFramebuffers(1, &Fbo); if (Tex) glDeleteTextures(1, &Tex); }
    };

    SGNodePreviews::SGNodePreviews() = default;
    SGNodePreviews::~SGNodePreviews() { m_Entries.clear(); if (m_Vao) glDeleteVertexArrays(1, &m_Vao); }
    void SGNodePreviews::Clear() { m_Entries.clear(); m_Revision = (size_t)-1; }

    bool SGNodePreviews::Previewable(const ShaderGraph& g, const SGNode& n) {
        if (n.Type == "Output") return false;
        const auto outs = ShaderGraphOutputs(g, n);
        return !outs.empty() && outs[0].Type != 5;                 // dynamic outputs too (the compiler knows their size)
    }

    uint32_t SGNodePreviews::Texture(int nodeId) const {
        auto it = m_Entries.find(nodeId);
        return it != m_Entries.end() && it->second->Program && it->second->C.Ok ? it->second->Tex : 0;
    }

    void SGNodePreviews::Update(const ShaderGraph& g, size_t revision, float time,
                                const std::function<std::shared_ptr<Texture2D>(const SGParam&)>& textureFor, int budget) {
        // The code of every previewable node (only when the graph changed)
        if (revision != m_Revision) {
            m_Revision = revision;
            std::set<int> alive;
            for (const SGNode& n : g.Nodes) {
                if (!Previewable(g, n)) continue;
                alive.insert(n.Id);
                auto& e = m_Entries[n.Id];
                if (!e) e = std::make_unique<Entry>();
                e->C = CompileShaderGraphPreview(g, n.Id, 0);
                if (e->C.Code != e->Code) { e->Code = e->C.Code; e->Dirty = true; }
            }
            for (auto it = m_Entries.begin(); it != m_Entries.end();) it = alive.count(it->first) ? std::next(it) : m_Entries.erase(it);
        }
        // Build a few programs
        for (auto& [id, e] : m_Entries) {
            if (budget <= 0) break;
            if (!e->Dirty) continue;
            e->Dirty = false;
            if (!e->C.Ok) { e->Program.reset(); continue; }
            Shader::Injection inj{ "", e->Code, "", "" };
            auto s = std::make_unique<Shader>(g_ShaderDir + "tonemap.vert", g_ShaderDir + "sg_preview.frag", std::move(inj));
            s->QuietMissing = true;
            if (s->Valid()) e->Program = std::move(s);
            else e->Program.reset();
            budget--;
        }
        // Draw them all
        GLint prevFbo = 0, prevProgram = 0, viewport[4];
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        glGetIntegerv(GL_VIEWPORT, viewport);
        const GLboolean depth = glIsEnabled(GL_DEPTH_TEST), blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE), scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
        if (!m_Vao) glCreateVertexArrays(1, &m_Vao);
        glBindVertexArray(m_Vao);
        glViewport(0, 0, Size, Size);
        for (auto& [id, e] : m_Entries) {
            if (!e->Program || !e->C.Ok) continue;
            if (!e->Tex) {
                glCreateTextures(GL_TEXTURE_2D, 1, &e->Tex);
                glTextureStorage2D(e->Tex, 1, GL_RGBA8, Size, Size);
                glTextureParameteri(e->Tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTextureParameteri(e->Tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glCreateFramebuffers(1, &e->Fbo);
                glNamedFramebufferTexture(e->Fbo, GL_COLOR_ATTACHMENT0, e->Tex, 0);
            }
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, e->Fbo);
            Shader& s = *e->Program;
            s.Bind();
            s.SetFloat("u_SceneWindTime", time);
            s.SetInt("u_Sphere", e->C.Spatial ? 1 : 0);
            for (size_t i = 0; i < e->C.Params.size(); i++) {
                const SGParam& p = e->C.Params[i];
                const std::string& u = e->C.Uniforms[i];
                if (p.Type == "texture") {
                    auto t = textureFor ? textureFor(p) : nullptr;
                    if (t) t->Bind((uint32_t)e->C.TextureUnits[i]);
                    else glBindTextureUnit((GLuint)e->C.TextureUnits[i], 0);
                }
                else if (p.Type == "color") s.SetVec3(u, glm::pow(glm::max(glm::vec3(p.Default), glm::vec3(0.0f)), glm::vec3(2.2f)));
                else if (p.Type == "float") s.SetFloat(u, p.Default.x);
                else if (p.Type == "vec2") s.SetVec2(u, glm::vec2(p.Default));
                else if (p.Type == "vec3") s.SetVec3(u, glm::vec3(p.Default));
                else s.SetVec4(u, p.Default);
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindVertexArray(0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevFbo);
        glUseProgram((GLuint)prevProgram);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (depth) glEnable(GL_DEPTH_TEST);
        if (blend) glEnable(GL_BLEND);
        if (cull) glEnable(GL_CULL_FACE);
        if (scissor) glEnable(GL_SCISSOR_TEST);
    }

} // namespace Engine
