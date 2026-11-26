#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

namespace Engine {

    class Shader {
    public:
        Shader(const std::string& vertexPath, const std::string& fragmentPath);
        ~Shader();

        Shader(const Shader&) = delete;
        Shader& operator=(const Shader&) = delete;

        void Bind() const;
        static void Unbind();

        // Recompile from disk. If compilation fails, the old program is kept.
        bool Reload();

        void SetInt(const std::string& name, int value);
        void SetFloat(const std::string& name, float value);
        void SetVec2(const std::string& name, const glm::vec2& value);
        void SetVec3(const std::string& name, const glm::vec3& value);
        void SetVec4(const std::string& name, const glm::vec4& value);
        void SetIVec4(const std::string& name, const glm::ivec4& value);
        void SetMat4(const std::string& name, const glm::mat4& value);
        void SetMat4Array(const std::string& name, const glm::mat4* values, uint32_t count);   // e.g. "u_Bones"
        void SetVec4Array(const std::string& name, const glm::vec4* values, uint32_t count);   // e.g. "u_Ripples"

        // Don't warn about uniforms this program lacks (it is given a shared set, e.g. Scene::SetLightingUniforms)
        bool QuietMissing = false;
        // Which program of a shader graph stands in for this one (SGProgram::Get): 0 lit, 1 depth pre-pass, 2 point
        // shadow, 3 sun shadow; -1 = none (graph batches draw with this shader itself)
        int GraphVariant = -1;
        // Generated code put into the fragment shader (shader graphs): Defines go right after the #version line,
        // Code replaces the line holding "// @GRAPH_CODE" (if the file has none, after the defines)
        struct Injection { std::string Defines, Code, VertexDefines, VertexCode; };   // Vertex*: the same into the vertex shader
        Shader(const std::string& vertexPath, const std::string& fragmentPath, Injection injection);
        // A compute program from one file (glDispatchCompute after Bind); Reload works the same
        static std::unique_ptr<Shader> CreateCompute(const std::string& computePath);
        // A shader compiled on first use (Bind): until then the values set on it are kept and given to it then. The web
        // build makes the shaders a game may never need this way (a browser compiles slowly: start-up time)
        static std::unique_ptr<Shader> CreateLazy(const std::string& vertexPath, const std::string& fragmentPath, Injection injection = {});
        bool Valid() const { return m_Pending || m_Program != 0; }
        const std::string& LastError() const { return m_Error; }   // the compile / link log of the last failure

        // Remember every value set (not arrays), so ReplayInto can give them to another program of the same kind:
        // a shader graph's lit program gets the lighting the plain lit shader was given this pass
        bool Record = false;
        void ReplayInto(Shader& other) const;

    private:
        uint32_t Compile(const std::string& vertexPath, const std::string& fragmentPath);
        struct Value { int Kind = 0; glm::vec4 V{ 0.0f }; glm::ivec4 I{ 0 }; glm::mat4 M{ 1.0f }; };   // 1 int 2 float 3 vec2 4 vec3 5 vec4 6 ivec4 7 mat4
        void Remember(const std::string& name, const Value& v) { if (Record) m_Values[name] = v; }
        // A value for a lazy shader that isn't compiled yet: kept (true = don't set it now)
        bool Hold(const std::string& name, const Value& v) { Remember(name, v); if (!m_Pending) return false; m_PendingValues[name] = v; return true; }
        void EnsureCompiled() const;
        bool m_Pending = false;
        std::unordered_map<std::string, Value> m_PendingValues;
        std::unordered_map<std::string, Value> m_Values;
        Injection m_Injection;
        std::string m_Error;
        int GetUniformLocation(const std::string& name);

        Shader() = default;
        uint32_t CompileCompute(const std::string& path);
        bool m_Compute = false;
        uint32_t m_Program = 0;
        std::string m_VertexPath, m_FragmentPath;
        std::unordered_map<std::string, int> m_UniformCache;
    };

} // namespace Engine