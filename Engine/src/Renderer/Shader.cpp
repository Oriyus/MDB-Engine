#include "Renderer/Shader.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <sstream>

namespace Engine {

    static std::string ReadFile(const std::string& path) {
        std::ifstream file(path, std::ios::in | std::ios::binary);
        if (!file) {
            ENGINE_ERROR("Could not open shader file: {}", path);
            return {};
        }
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    // Replaces each line `#include "name"` with that file (next to the including one), once per file. Line numbers in
    // compile errors after an include are off by its length.
    static std::string ResolveIncludes(const std::string& path, const std::string& src, int depth = 0) {
        if (depth > 8 || src.find("#include") == std::string::npos) return src;
        const size_t slash = path.find_last_of("/\\");
        const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
        std::string out;
        size_t pos = 0;
        while (pos < src.size()) {
            size_t eol = src.find('\n', pos);
            if (eol == std::string::npos) eol = src.size();
            const std::string line = src.substr(pos, eol - pos);
            const size_t inc = line.find("#include");
            const size_t q0 = line.find('"'), q1 = q0 == std::string::npos ? q0 : line.find('"', q0 + 1);
            if (inc != std::string::npos && line.find_first_not_of(" \t") == inc && q1 != std::string::npos) {
                const std::string file = dir + line.substr(q0 + 1, q1 - q0 - 1);
                out += ResolveIncludes(file, ReadFile(file), depth + 1) + "\n";
            }
            else out += line + "\n";
            pos = eol + 1;
        }
        return out;
    }

    static uint32_t CompileStage(GLenum type, const std::string& source, const std::string& path, std::string* error = nullptr) {
        uint32_t shader = glCreateShader(type);
        const char* src = source.c_str();
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        int ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            ENGINE_ERROR("Shader compile error in {}:\n{}", path, log);
            if (error) *error = log;
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath)
        : m_VertexPath(vertexPath), m_FragmentPath(fragmentPath) {
        m_Program = Compile(vertexPath, fragmentPath);
    }

    Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath, Injection injection)
        : m_VertexPath(vertexPath), m_FragmentPath(fragmentPath), m_Injection(std::move(injection)) {
        m_Program = Compile(vertexPath, fragmentPath);
    }

    std::unique_ptr<Shader> Shader::CreateCompute(const std::string& path) {
        std::unique_ptr<Shader> s(new Shader());
        s->m_Compute = true;
        s->m_VertexPath = path;
        s->m_Program = s->CompileCompute(path);
        return s;
    }

    uint32_t Shader::CompileCompute(const std::string& path) {
        std::string src = ReadFile(path);
        if (src.empty()) return 0;
        src = ResolveIncludes(path, src);
        m_Error.clear();
        uint32_t cs = CompileStage(GL_COMPUTE_SHADER, src, path, &m_Error);
        if (!cs) return 0;
        uint32_t program = glCreateProgram();
        glAttachShader(program, cs);
        glLinkProgram(program);
        glDeleteShader(cs);
        int ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            ENGINE_ERROR("Shader link error in {}:\n{}", path, log);
            m_Error = log;
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    void Shader::ReplayInto(Shader& o) const {
        for (const auto& [name, v] : m_Values) {
            switch (v.Kind) {
            case 1: o.SetInt(name, v.I.x); break;
            case 2: o.SetFloat(name, v.V.x); break;
            case 3: o.SetVec2(name, glm::vec2(v.V)); break;
            case 4: o.SetVec3(name, glm::vec3(v.V)); break;
            case 5: o.SetVec4(name, v.V); break;
            case 6: o.SetIVec4(name, v.I); break;
            case 7: o.SetMat4(name, v.M); break;
            default: break;
            }
        }
    }

    Shader::~Shader() {
        if (m_Program) glDeleteProgram(m_Program);
    }

    uint32_t Shader::Compile(const std::string& vertexPath, const std::string& fragmentPath) {
        std::string vsSource = ReadFile(vertexPath);
        std::string fsSource = ReadFile(fragmentPath);
        if (vsSource.empty() || fsSource.empty()) return 0;
        vsSource = ResolveIncludes(vertexPath, vsSource);
        fsSource = ResolveIncludes(fragmentPath, fsSource);

        // A shader graph's code: defines after the #version line, the code at "// @GRAPH_CODE" (else after the defines)
        auto inject = [](std::string& src, const std::string& defines, const std::string& code) {
            if (defines.empty() && code.empty()) return;
            const size_t eol = src.find('\n');
            std::string head = src.substr(0, eol + 1) + defines + "\n";
            std::string rest = eol == std::string::npos ? std::string() : src.substr(eol + 1);
            const std::string marker = "// @GRAPH_CODE";
            const size_t mark = rest.find(marker);
            if (mark != std::string::npos) rest.replace(mark, marker.size(), code);
            else head += code + "\n";
            src = head + rest;
        };
        inject(fsSource, m_Injection.Defines, m_Injection.Code);
        inject(vsSource, m_Injection.VertexDefines, m_Injection.VertexCode);
        m_Error.clear();
        uint32_t vs = CompileStage(GL_VERTEX_SHADER, vsSource, vertexPath, &m_Error);
        uint32_t fs = CompileStage(GL_FRAGMENT_SHADER, fsSource, fragmentPath, &m_Error);
        if (!vs || !fs) {
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
            return 0;
        }

        uint32_t program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);

        glDeleteShader(vs);
        glDeleteShader(fs);

        int ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            ENGINE_ERROR("Shader link error:\n{}", log);
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    bool Shader::Reload() {
        uint32_t newProgram = m_Compute ? CompileCompute(m_VertexPath) : Compile(m_VertexPath, m_FragmentPath);
        if (!newProgram) {
            ENGINE_WARN("Shader reload failed, keeping previous version");
            return false;
        }
        if (m_Program) glDeleteProgram(m_Program);
        m_Program = newProgram;
        m_UniformCache.clear();
        ENGINE_INFO("Shader reloaded");
        return true;
    }

    void Shader::Bind() const { glUseProgram(m_Program); }
    void Shader::Unbind() { glUseProgram(0); }

    int Shader::GetUniformLocation(const std::string& name) {
        auto it = m_UniformCache.find(name);
        if (it != m_UniformCache.end()) return it->second;

        int loc = glGetUniformLocation(m_Program, name.c_str());
        if (loc == -1 && !QuietMissing) ENGINE_WARN("Uniform '{}' not found (or optimized out)", name);
        m_UniformCache[name] = loc;
        return loc;
    }

    // These use glProgramUniform*, so the shader doesn't need to be bound first
    void Shader::SetInt(const std::string& name, int v) { if (Record) { Value x; x.Kind = 1; x.I.x = v; Remember(name, x); } glProgramUniform1i(m_Program, GetUniformLocation(name), v); }
    void Shader::SetFloat(const std::string& name, float v) { if (Record) { Value x; x.Kind = 2; x.V.x = v; Remember(name, x); } glProgramUniform1f(m_Program, GetUniformLocation(name), v); }
    void Shader::SetVec2(const std::string& name, const glm::vec2& v) { if (Record) { Value x; x.Kind = 3; x.V = glm::vec4(v, 0.0f, 0.0f); Remember(name, x); } glProgramUniform2fv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetVec3(const std::string& name, const glm::vec3& v) { if (Record) { Value x; x.Kind = 4; x.V = glm::vec4(v, 0.0f); Remember(name, x); } glProgramUniform3fv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetVec4(const std::string& name, const glm::vec4& v) { if (Record) { Value x; x.Kind = 5; x.V = v; Remember(name, x); } glProgramUniform4fv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetIVec4(const std::string& name, const glm::ivec4& v) { if (Record) { Value x; x.Kind = 6; x.I = v; Remember(name, x); } glProgramUniform4iv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetMat4(const std::string& name, const glm::mat4& v) { if (Record) { Value x; x.Kind = 7; x.M = v; Remember(name, x); } glProgramUniformMatrix4fv(m_Program, GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(v)); }
    void Shader::SetMat4Array(const std::string& name, const glm::mat4* v, uint32_t count) {
        if (count > 0) glProgramUniformMatrix4fv(m_Program, GetUniformLocation(name), (GLsizei)count, GL_FALSE, glm::value_ptr(v[0]));
    }

} // namespace Engine