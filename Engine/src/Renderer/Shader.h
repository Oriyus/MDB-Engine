#pragma once
#include <cstdint>
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

        // Don't warn about uniforms this program lacks (it is given a shared set, e.g. Scene::SetLightingUniforms)
        bool QuietMissing = false;

    private:
        uint32_t Compile(const std::string& vertexPath, const std::string& fragmentPath);
        int GetUniformLocation(const std::string& name);

        uint32_t m_Program = 0;
        std::string m_VertexPath, m_FragmentPath;
        std::unordered_map<std::string, int> m_UniformCache;
    };

} // namespace Engine