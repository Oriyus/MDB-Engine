#include "Renderer/Camera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace Engine {

    Camera::Camera(float fov, float aspect, float nearPlane, float farPlane)
        : m_Fov(fov), m_Aspect(aspect), m_Near(nearPlane), m_Far(farPlane) {
        RecalcProjection();
        RecalcView();
    }

    void Camera::SetAspect(float aspect) {
        m_Aspect = aspect;
        RecalcProjection();
    }

    // Keep the viewing direction, back up until the sphere fits, and make its center the pivot
    void Camera::Focus(const glm::vec3& center, float radius) {
        float vHalf = glm::radians(m_Fov) * 0.5f;
        float hHalf = std::atan(std::tan(vHalf) * m_Aspect);   // narrow panels are limited by width
        float half = std::min(vHalf, hHalf);

        FocusDistance = std::clamp(radius / std::sin(half) * 1.1f, 0.05f, 400.0f);
        Position = center - m_Front * FocusDistance;
        RecalcView();
    }

    void Camera::UpdateFront() {
        glm::vec3 front;
        front.x = std::cos(glm::radians(m_Yaw)) * std::cos(glm::radians(m_Pitch));
        front.y = std::sin(glm::radians(m_Pitch));
        front.z = std::sin(glm::radians(m_Yaw)) * std::cos(glm::radians(m_Pitch));
        m_Front = glm::normalize(front);
    }

    glm::vec3 Camera::GetRight() const {
        return glm::normalize(glm::cross(m_Front, glm::vec3(0, 1, 0)));
    }

    void Camera::Refresh() {
        UpdateFront();
        RecalcView();
    }

    // Tumble: rotate the camera around the pivot, keeping the pivot fixed on screen
    void Camera::Orbit(float dx, float dy) {
        glm::vec3 pivot = Position + m_Front * FocusDistance;

        m_Yaw += dx * OrbitSensitivity;
        m_Pitch -= dy * OrbitSensitivity;
        m_Pitch = std::clamp(m_Pitch, -89.0f, 89.0f);

        UpdateFront();
        Position = pivot - m_Front * FocusDistance;
        RecalcView();
    }

    // Track: slide the camera in its own plane. Scaled so the scene at the pivot
    // distance follows the cursor 1:1, whatever the zoom level.
    void Camera::Pan(float dx, float dy, float viewportHeight) {
        float worldPerPixel = 2.0f * FocusDistance * std::tan(glm::radians(m_Fov) * 0.5f)
            / std::max(viewportHeight, 1.0f);
        glm::vec3 right = GetRight();
        glm::vec3 up = glm::normalize(glm::cross(right, m_Front));

        Position += (-right * dx + up * dy) * worldPerPixel;
        RecalcView();
    }

    // Dolly: move toward or away from the pivot. Exponential, so each step is a
    // fixed percentage of the distance and zooming feels the same near or far.
    void Camera::Dolly(float amount) {
        float newDistance = std::clamp(FocusDistance * std::exp(-amount), 0.05f, 400.0f);
        Position += m_Front * (FocusDistance - newDistance);
        FocusDistance = newDistance;
        RecalcView();
    }

    void Camera::RecalcView() {
        m_View = glm::lookAt(Position, Position + m_Front, glm::vec3(0, 1, 0));
    }

    void Camera::RecalcProjection() {
        m_Projection = glm::perspective(glm::radians(m_Fov), m_Aspect, m_Near, m_Far);
    }

} // namespace Engine