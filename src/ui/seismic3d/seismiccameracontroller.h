#pragma once

#include <glm/glm.hpp>

namespace seismic {

class SeismicCameraController {
public:
    enum class PresetView {
        Isometric,
        Top,
        Front,
        Side,
        Reset
    };

    SeismicCameraController();

    [[nodiscard]] glm::mat4 BuildViewMatrix() const;
    [[nodiscard]] glm::mat4 BuildProjectionMatrix(float aspect, float fovDeg = 45.0f, float zNear = 0.1f, float zFar = 1000.0f) const;

    void Reset();
    void SetTopView();
    void SetIsometricView();
    void SetFrontView();
    void SetSideView();
    void ApplyPreset(PresetView preset);

    void FitToBounds(const glm::vec3 &minValue, const glm::vec3 &maxValue, float viewportAspect = 1.0f);

    void Rotate(float dx, float dy);
    void Pan(float dx, float dy);
    void Zoom(float wheelDelta);

    [[nodiscard]] float Yaw() const { return yaw_; }
    [[nodiscard]] float Pitch() const { return pitch_; }
    [[nodiscard]] float Distance() const { return distance_; }
    [[nodiscard]] const glm::vec3 &Target() const { return target_; }

    void SetYaw(float yaw) { yaw_ = yaw; }
    void SetPitch(float pitch);
    void SetDistance(float dist);
    void SetTarget(const glm::vec3 &target) { target_ = target; }

private:
    float yaw_ = -45.0f;
    float pitch_ = 35.0f;
    float distance_ = 9.0f;
    glm::vec3 target_{0.0f};
};

} // namespace seismic
