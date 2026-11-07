#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine {

    class Camera {
    public:
        Camera(float fovDegrees = 60.0f, float aspect = 16.0f / 9.0f,
            float nearPlane = 0.1f, float farPlane = 500.0f);

        void SetAspect(float aspect);

        // Fly mode: WASD to move (Q/E down/up), mouse to look
        void Refresh();

        void Focus(const glm::vec3& center, float radius);   // frame a bounding sphere

        // Maya-style navigation around a pivot that sits FocusDistance in front of the camera
        void Orbit(float dx, float dy);                       // mouse delta in pixels
        void Pan(float dx, float dy, float viewportHeight);   // mouse delta in pixels
        void Dolly(float amount);                             // > 0 moves closer

        const glm::mat4& GetView() const { return m_View; }
        const glm::mat4& GetProjection() const { return m_Projection; }
        glm::mat4 GetViewProjection() const { return m_Projection * m_View; }
        glm::vec3 GetFront() const { return m_Front; }
        glm::vec3 GetRight() const;

        float GetYaw() const { return m_Yaw; }       // degrees
        float GetPitch() const { return m_Pitch; }   // degrees, clamped to +-89
        void SetOrientation(float yawDegrees, float pitchDegrees);

        // Drive the camera from an entity (game cameras): position + rotation, looking along local -Z.
        // Roll is kept. Refresh/Orbit/Pan would go back to yaw/pitch, so don't mix the two.
        void SetPose(const glm::vec3& position, const glm::quat& rotation);
        void SetLens(float fovDegrees, float nearPlane, float farPlane);

        glm::vec3 Position{ 0.0f, 0.0f, 4.0f };
        float OrbitSensitivity = 0.3f;   // degrees per pixel
        float FocusDistance = 6.0f;      // distance from the camera to the orbit pivot

    private:
        void UpdateFront();
        void RecalcView();
        void RecalcProjection();

        float m_Yaw = -90.0f;    // -90 so we start looking down -Z
        float m_Pitch = 0.0f;
        float m_Fov, m_Aspect, m_Near, m_Far;
        glm::vec3 m_Front{ 0.0f, 0.0f, -1.0f };
        glm::mat4 m_View{ 1.0f };
        glm::mat4 m_Projection{ 1.0f };
    };

} // namespace Engine