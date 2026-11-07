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

    static uint32_t CompileStage(GLenum type, const std::string& source, const std::string& path) {
        uint32_t shader = glCreateShader(type);
        const char* src = source.c_str();
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        int ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            ENGINE_ERROR("Shader compile error in {}:\n{}", path, log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    Shader::Shader(const std::string& vertexPath, const std::string& fragmentPath)
        : m_VertexPath(vertexPath), m_FragmentPath(fragmentPath) {
        m_Program = Compile(vertexPath, fragmentPath);
    }

    Shader::~Shader() {
        if (m_Program) glDeleteProgram(m_Program);
    }

    uint32_t Shader::Compile(const std::string& vertexPath, const std::string& fragmentPath) {
        std::string vsSource = ReadFile(vertexPath);
        std::string fsSource = ReadFile(fragmentPath);
        if (vsSource.empty() || fsSource.empty()) return 0;

        uint32_t vs = CompileStage(GL_VERTEX_SHADER, vsSource, vertexPath);
        uint32_t fs = CompileStage(GL_FRAGMENT_SHADER, fsSource, fragmentPath);
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
        uint32_t newProgram = Compile(m_VertexPath, m_FragmentPath);
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
        if (loc == -1) ENGINE_WARN("Uniform '{}' not found (or optimized out)", name);
        m_UniformCache[name] = loc;
        return loc;
    }

    // These use glProgramUniform*, so the shader doesn't need to be bound first
    void Shader::SetInt(const std::string& name, int v) { glProgramUniform1i(m_Program, GetUniformLocation(name), v); }
    void Shader::SetFloat(const std::string& name, float v) { glProgramUniform1f(m_Program, GetUniformLocation(name), v); }
    void Shader::SetVec3(const std::string& name, const glm::vec3& v) { glProgramUniform3fv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetVec4(const std::string& name, const glm::vec4& v) { glProgramUniform4fv(m_Program, GetUniformLocation(name), 1, glm::value_ptr(v)); }
    void Shader::SetMat4(const std::string& name, const glm::mat4& v) { glProgramUniformMatrix4fv(m_Program, GetUniformLocation(name), 1, GL_FALSE, glm::value_ptr(v)); }
    void Shader::SetMat4Array(const std::string& name, const glm::mat4* v, uint32_t count) {
        if (count > 0) glProgramUniformMatrix4fv(m_Program, GetUniformLocation(name), (GLsizei)count, GL_FALSE, glm::value_ptr(v[0]));
    }

} // namespace Engine