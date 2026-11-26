#include "Renderer/Shader.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <sstream>
#include <cstdlib>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <regex>
#include <vector>
#endif

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

#ifdef __EMSCRIPTEN__
    // The web build: the engine's GLSL 4.50 as GLSL ES 3.00 (WebGL2). The #version line becomes ES with MDB_WEB defined
    // (shaders branch on it where WebGL2 lacks something, e.g. storage buffers: web_storage.glsl) and default
    // precisions; `layout(binding = N)` on samplers is dropped and remembered (ES has no such qualifier: the units are set
    // after linking, as for u_SB<N>, the storage buffers' textures); `layout(location = N)` on varyings is dropped (ES
    // allows it only on vertex inputs and fragment outputs; varyings match by name). bitfieldReverse is ES 3.10.
    struct WebBindings { std::vector<std::pair<std::string, int>> Samplers; };

    static std::string WebSource(const std::string& src, GLenum type, WebBindings& bindings) {
        static const char* kHeader =
            "#version 300 es\n"
            "#define MDB_WEB 1\n"
            "precision highp float;\nprecision highp int;\n"
            "precision highp sampler2D;\nprecision highp sampler2DArray;\nprecision highp sampler2DShadow;\n"
            "precision highp sampler2DArrayShadow;\nprecision highp samplerCube;\nprecision highp sampler3D;\n"
            "precision highp usampler2D;\nprecision highp isampler2D;\n"
            "uint MDB_BitReverse(uint v) {\n"
            "    v = ((v >> 1u) & 0x55555555u) | ((v & 0x55555555u) << 1u);\n"
            "    v = ((v >> 2u) & 0x33333333u) | ((v & 0x33333333u) << 2u);\n"
            "    v = ((v >> 4u) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4u);\n"
            "    v = ((v >> 8u) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8u);\n"
            "    return (v >> 16u) | (v << 16u);\n"
            "}\n"
            "int MDB_BitReverse(int v) { return int(MDB_BitReverse(uint(v))); }\n"
            "#define bitfieldReverse MDB_BitReverse\n";
        std::string out;
        out.reserve(src.size() + 1024);
        size_t pos = 0;
        bool first = true;
        static const std::regex binding(R"(layout\s*\(\s*binding\s*=\s*(\d+)\s*\)\s*uniform\s+((?:highp\s+|mediump\s+|lowp\s+)?\w+)\s+(\w+))");
        static const std::regex location(R"(layout\s*\(\s*location\s*=\s*\d+\s*\)\s*((?:flat\s+|smooth\s+|centroid\s+)*)(in|out)\b)");
        while (pos < src.size()) {
            size_t eol = src.find('\n', pos);
            if (eol == std::string::npos) eol = src.size();
            std::string line = src.substr(pos, eol - pos);
            pos = eol + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (first && line.rfind("#version", 0) == 0) { out += kHeader; first = false; continue; }
            first = false;
            if (line.find("layout") != std::string::npos) {
                std::smatch m;
                if (std::regex_search(line, m, binding)) {
                    bindings.Samplers.emplace_back(m[3].str(), std::stoi(m[1].str()));
                    line = std::string(m.prefix()) + "uniform " + m[2].str() + " " + m[3].str() + std::string(m.suffix());
                }
                if (std::regex_search(line, m, location)) {
                    const bool varying = (type == GL_VERTEX_SHADER && m[2] == "out") || (type == GL_FRAGMENT_SHADER && m[2] == "in");
                    if (varying) line = std::string(m.prefix()) + m[1].str() + m[2].str() + std::string(m.suffix());
                }
            }
            out += line;
            out += '\n';
        }
        return out;
    }

    // After linking: every sampler that had a layout(binding) and every storage buffer texture gets its unit
    static void ApplyWebBindings(uint32_t program, const WebBindings& bindings) {
        for (const auto& [name, unit] : bindings.Samplers) {
            const GLint loc = glGetUniformLocation(program, name.c_str());
            if (loc >= 0) glProgramUniform1i(program, loc, unit);
        }
        for (int b = 0; b <= 10; b++) {
            const int unit = mdbgl_StorageUnit((GLuint)b);
            if (unit < 0) continue;
            const GLint loc = glGetUniformLocation(program, ("u_SB" + std::to_string(b)).c_str());
            if (loc >= 0) glProgramUniform1i(program, loc, unit);
        }
    }
#endif

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
            // The lines the log names ("0:123:" on ANGLE / WebGL, "0(123)" on NVIDIA), from the source as compiled
            // (includes resolved): the file's own line numbers are off after an include
            std::string context;
            for (const char* p = log; *p; p++) {
                if (p[0] != '0' || (p[1] != ':' && p[1] != '(')) continue;
                const int lineNo = std::atoi(p + 2);
                if (lineNo <= 0) continue;
                size_t at = 0;
                for (int l = 1; l < lineNo && at != std::string::npos; l++) at = source.find('\n', at) == std::string::npos ? std::string::npos : source.find('\n', at) + 1;
                if (at == std::string::npos) continue;
                context += "  " + std::to_string(lineNo) + ": " + source.substr(at, source.find('\n', at) - at) + "\n";
                if (context.size() > 2000) break;
            }
            ENGINE_ERROR("Shader compile error in {}:\n{}{}", path, log, context);
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
#ifdef __EMSCRIPTEN__
        ENGINE_WARN("Compute shaders are not available in WebGL2 ({})", path);
        return 0;
#endif
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
#ifdef __EMSCRIPTEN__
        const double compileStart = emscripten_get_now();
#endif
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
#ifdef __EMSCRIPTEN__
        WebBindings webBindings;
        vsSource = WebSource(vsSource, GL_VERTEX_SHADER, webBindings);
        fsSource = WebSource(fsSource, GL_FRAGMENT_SHADER, webBindings);
#endif
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
            ENGINE_ERROR("Shader link error ({} + {}):\n{}", vertexPath, fragmentPath, log);
            m_Error = log;
            glDeleteProgram(program);
            return 0;
        }
#ifdef __EMSCRIPTEN__
        ApplyWebBindings(program, webBindings);
        // A browser compiles shaders slowly (ANGLE translates them for Direct3D): show which ones cost the start-up
        if (const double ms = emscripten_get_now() - compileStart; ms > 200.0)
            ENGINE_INFO("Shader {} + {}: {:.0f} ms", vertexPath.substr(vertexPath.find_last_of("/\\") + 1), fragmentPath.substr(fragmentPath.find_last_of("/\\") + 1), ms);
#endif
        return program;
    }

    bool Shader::Reload() {
        if (m_Pending) return true;           // a lazy shader not used yet reads the files when it is
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

    std::unique_ptr<Shader> Shader::CreateLazy(const std::string& vertexPath, const std::string& fragmentPath, Injection injection) {
        std::unique_ptr<Shader> s(new Shader());
        s->m_VertexPath = vertexPath;
        s->m_FragmentPath = fragmentPath;
        s->m_Injection = std::move(injection);
        s->m_Pending = true;
        return s;
    }

    // A lazy shader's first use: compile, then set what it was given meanwhile
    void Shader::EnsureCompiled() const {
        if (!m_Pending) return;
        Shader& self = const_cast<Shader&>(*this);
        self.m_Pending = false;
        self.m_Program = self.Compile(m_VertexPath, m_FragmentPath);
        std::unordered_map<std::string, Value> values;
        values.swap(self.m_PendingValues);
        for (const auto& [name, v] : values) {
            switch (v.Kind) {
            case 1: self.SetInt(name, v.I.x); break;
            case 2: self.SetFloat(name, v.V.x); break;
            case 3: self.SetVec2(name, glm::vec2(v.V)); break;
            case 4: self.SetVec3(name, glm::vec3(v.V)); break;
            case 5: self.SetVec4(name, v.V); break;
            case 6: self.SetIVec4(name, v.I); break;
            case 7: self.SetMat4(name, v.M); break;
            default: break;
            }
        }
    }

    void Shader::Bind() const { EnsureCompiled(); glUseProgram(m_Program); }
    void Shader::Unbind() { glUseProgram(0); }

    int Shader::GetUniformLocation(const std::string& name) {
        auto it = m_UniformCache.find(name);
        if (it != m_UniformCache.end()) return it->second;

        int loc = glGetUniformLocation(m_Program, name.c_str());
#ifdef __EMSCRIPTEN__
        // The web shaders leave out what WebGL2 can't do (the uniforms of those features are expected to be missing)
        if (false)
#else
        if (loc == -1 && !QuietMissing)
#endif
            ENGINE_WARN("Uniform '{}' not found (or optimized out)", name);
        m_UniformCache[name] = loc;
        return loc;
    }

    // These use glProgramUniform*, so the shader doesn't need to be bound first
    void Shader::SetInt(const std::string& name, int v) { if (Record || m_Pending) { Value x; x.Kind = 1; x.I.x = v; if (Hold(name, x)) return; } glProgramUniform1i(m_Program, GetUniformLocation(name), v); }
    void Shader::SetFloat(const std::string& name, float v) { if (Record || m_Pending) { Value x; x.Kind = 2; x.V.x = v; if (Hold(name, x)) return; } glProgramUniform1f(m_Program, GetUniformLocation(name), v); }
    void Shader::SetVec2(const std::string& name, const glm::vec2& v) { if (Record || m_Pending) { Value x; x.Kind = 3; x.V = glm::vec4(v, 0.0f, 0.0f); if (Hold(name, x)) return; } glProgramUniform2fv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetVec3(const std::string& name, const glm::vec3& v) { if (Record || m_Pending) { Value x; x.Kind = 4; x.V = glm::vec4(v, 0.0f); if (Hold(name, x)) return; } glProgramUniform3fv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetVec4(const std::string& name, const glm::vec4& v) { if (Record || m_Pending) { Value x; x.Kind = 5; x.V = v; if (Hold(name, x)) return; } glProgramUniform4fv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetIVec4(const std::string& name, const glm::ivec4& v) { if (Record || m_Pending) { Value x; x.Kind = 6; x.I = v; if (Hold(name, x)) return; } glProgramUniform4iv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetMat4(const std::string& name, const glm::mat4& v) { if (Record || m_Pending) { Value x; x.Kind = 7; x.M = v; if (Hold(name, x)) return; } glProgramUniformMatrix4fv(m_Program, GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(v)); }
    void Shader::SetMat4Array(const std::string& name, const glm::mat4* v, uint32_t count) {
        if (m_Pending) return;                // (set right before the draws that need them, after Bind)
        if (count > 0) glProgramUniformMatrix4fv(m_Program, GetUniformLocation(name), (GLsizei)count, GL_FALSE, glm::value_ptr(v[0]));
    }
    void Shader::SetVec4Array(const std::string& name, const glm::vec4* v, uint32_t count) {
        if (m_Pending) return;                // (set right before the draws that need them, after Bind)
        if (count > 0) glProgramUniform4fv(m_Program, GetUniformLocation(name), (GLsizei)count, glm::value_ptr(v[0]));
    }

} // namespace Engine