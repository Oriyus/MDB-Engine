#pragma once
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Shader;
    class Texture2D;

    // ---------------------------------------------------------------- the file
    // A shader graph, Assets/Shaders/<name>.shadergraph (JSON), edited in the Shader Graph Editor and used by material
    // files ("shader": "Shaders/x.shadergraph"). Nodes are wired into the Output node, whose inputs are a surface:
    // base colour, alpha, metallic, roughness, specular, normal (tangent space) and emission. The graph describes the
    // surface only: lights, shadows, image-based light, fog and the rest of lit.frag stay the engine's, so a graph
    // material is lit and shadowed like any other.
    //
    //   { "version": 1, "settings": { "unlit": false },
    //     "params": [ { "name": "Tint", "type": "color", "default": [1, 1, 1] },
    //                 { "name": "Albedo", "type": "texture", "default": "Textures/x.png", "linear": false } ],
    //     "nodes":  [ { "id": 1, "type": "Output", "pos": [400, 0], "values": { "Roughness": [0.5] } }, ... ],
    //     "links":  [ { "from": [2, "RGB"], "to": [1, "BaseColor"] } ] }
    //
    // Params are exposed to materials: a material using the graph sets them ("params": { "Tint": [1, 0, 0] }).
    // Types: float, vec2, vec3, vec4, color (sRGB in the file and pickers, linear in the shader), texture.
    // A node's "values" hold the literals of its unconnected inputs (1-4 numbers) and its settings ("param", "texture").
    struct SGParam {
        std::string Name;
        std::string Type = "float";               // float vec2 vec3 vec4 color texture
        glm::vec4 Default{ 0.0f };                // numbers (color: sRGB rgb, a)
        std::string Texture;                      // texture: the default image (Assets-relative)
        bool Linear = false;                      // texture: data, not colour (normal, roughness, masks)
    };
    struct SGNode {
        int Id = 0;
        std::string Type;
        glm::vec2 Pos{ 0.0f };
        nlohmann::json Values = nlohmann::json::object();
    };
    struct SGLink {
        int FromNode = 0; std::string FromPin;
        int ToNode = 0;   std::string ToPin;
    };
    struct ShaderGraph {
        bool Unlit = false;                       // settings: its colour only, no lighting
        std::vector<SGParam> Params;
        std::vector<SGNode> Nodes;
        std::vector<SGLink> Links;

        nlohmann::json ToJson() const;
        static ShaderGraph FromJson(const nlohmann::json& j);   // lenient: bad entries are skipped
        static ShaderGraph Default();             // a new graph: an Output node fed by a Color property
        const SGNode* Find(int id) const;
        SGNode* Find(int id);
        int NextId() const;
        const SGParam* Param(const std::string& name) const;
    };

    // ---------------------------------------------------------------- the node library
    // Pin types: 1 float, 2 vec2, 3 vec3, 4 vec4, 5 texture; 0 = dynamic (the widest of the node's dynamic inputs).
    // A float goes into any vector (all components); a wider vector into a narrower one keeps its first components;
    // a narrower one into a wider one fills with 0 (and 1 for alpha).
    struct SGPinDef {
        std::string Name;
        int Type = 1;
        glm::vec4 Default{ 0.0f };
        std::string Expr;                         // an input's default as GLSL instead of Default ("IN.UV")
    };
    struct SGNodeDef {
        std::string Type, Category, Help;
        std::vector<SGPinDef> In, Out;
        std::vector<std::string> OutExpr;         // per output: GLSL with $0.. = inputs, $T = the dynamic type
        bool Special = false;                     // compiled by hand (Output, Property, constants, texture samples)
    };
    const std::vector<SGNodeDef>& ShaderGraphNodes();
    const SGNodeDef* ShaderGraphNode(const std::string& type);
    // The output pins of a node as placed (a Property's output takes its param's type)
    std::vector<SGPinDef> ShaderGraphOutputs(const ShaderGraph& g, const SGNode& n);
    const char* ShaderGraphTypeName(int type);    // "float" "vec2" ... "texture"

    // ---------------------------------------------------------------- compiling
    // The graph as GLSL put into lit.frag, depth_prepass.frag, shadow.frag and sun_shadow.frag at "// @GRAPH_CODE"
    // (with MDB_GRAPH defined): the structs SGIn / SGOut, the param uniforms and SGOut GraphSurface(SGIn IN).
    struct SGCompiled {
        bool Ok = false;
        std::string Code;
        std::vector<std::string> Errors;          // whole-graph problems
        std::map<int, std::string> NodeErrors;    // per node id
        std::vector<SGParam> Params;              // every uniform it reads: the exposed params, then hidden ones
        std::vector<std::string> Uniforms;        // per Params entry: its GLSL name
        std::vector<int> TextureUnits;            // per Params entry: the unit of a texture (else -1)
        bool WritesAlpha = false;                 // Alpha is connected: the depth pre-pass and shadows need the graph
        std::string VertexCode;                   // Vertex Offset connected: vec3 GraphVertexOffset(SGIn IN), for the vertex shaders
        bool HasVertexOffset = false;             // (then every pass draws the graph's programs, so depth and shadows move with it)
        bool Spatial = false;                     // previews: it depends on position / normal / view (shown on a sphere)
        bool Unlit = false;
    };
    SGCompiled CompileShaderGraph(const ShaderGraph& g);
    // One node output alone, for the editor's thumbnails: vec4 GraphPreview(SGIn IN) (a float as grey, a vec2 as red / green)
    SGCompiled CompileShaderGraphPreview(const ShaderGraph& g, int nodeId, int output);

    // ---------------------------------------------------------------- running it
    // The programs of one compiled graph: 0 lit (lit.vert + lit.frag), 1 depth pre-pass, 2 point shadow, 3 sun shadow.
    // Made on first use (a GL context is needed); shared by every graph with the same code.
    class SGProgram {
    public:
        explicit SGProgram(std::string code, std::string vertexCode = "");
        ~SGProgram();
        Shader* Get(int variant);                 // null if it failed to compile (Error says why)
        const std::string& Error() const { return m_Error; }
        const std::string& Code() const { return m_Code; }
        // Where the engine's shaders are (SceneRenderer sets it), and the program for some code (cached)
        static void SetShaderDir(const std::string& dir);
        static std::shared_ptr<SGProgram> For(const std::string& code, const std::string& vertexCode = "");
        static void ReloadAll();                  // F5: build every variant again
    private:
        std::string m_Code, m_VertexCode, m_Error;
        std::unique_ptr<Shader> m_Variants[4];
        bool m_Tried[4] = {};
    };

    // A material's use of a graph: the program and the values of its params (textures loaded)
    struct SGBinding {
        std::shared_ptr<SGProgram> Program;
        struct Value { std::string Uniform; int Kind = 1; glm::vec4 V{ 0.0f }; std::shared_ptr<Texture2D> Tex; int Unit = -1; };
        std::vector<Value> Values;
        bool WritesAlpha = false;
        bool HasVertexOffset = false;             // its programs move the mesh: every pass must use them
        void Apply(Shader& s) const;              // set the uniforms, bind the textures
    };

} // namespace Engine

namespace Engine {

    // The Shader Graph Editor's node thumbnails: each node's first output drawn into a small texture by its own program
    // (tonemap.vert + sg_preview.frag with CompileShaderGraphPreview's code). Programs are rebuilt when a node's code
    // changes, a few per Update so an edit doesn't stall; every node is redrawn each Update so Time nodes animate.
    // Params take the graph's defaults. Needs a GL context; restores the framebuffer, viewport and program it found.
    class SGNodePreviews {
    public:
        static constexpr int Size = 96;           // pixels
        SGNodePreviews();
        ~SGNodePreviews();
        SGNodePreviews(const SGNodePreviews&) = delete;
        SGNodePreviews& operator=(const SGNodePreviews&) = delete;

        // revision: anything that changes whenever the graph does. textureFor: the image of a texture param (or null).
        void Update(const ShaderGraph& g, size_t revision, float time,
                    const std::function<std::shared_ptr<Texture2D>(const SGParam&)>& textureFor, int compileBudget = 3);
        uint32_t Texture(int nodeId) const;       // the GL texture, 0 = none (yet)
        static bool Previewable(const ShaderGraph& g, const SGNode& n);
        void Clear();

    private:
        struct Entry;
        std::map<int, std::unique_ptr<Entry>> m_Entries;
        size_t m_Revision = (size_t)-1;
        uint32_t m_Vao = 0;
    };

} // namespace Engine
