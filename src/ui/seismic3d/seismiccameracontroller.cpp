// 层：视图
#include "seismiccameracontroller.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace seismic {

namespace {

constexpr float kMinCameraDistance = 0.2f;
constexpr float kMaxCameraDistance = 100000.0f;

} // namespace

SeismicCameraController::SeismicCameraController() {
    Reset();
}

glm::mat4 SeismicCameraController::BuildViewMatrix() const {
    const float yawRad = glm::radians(yaw_);
    const float pitchRad = glm::radians(pitch_);
    const glm::vec3 cameraPos(
        distance_ * std::cos(pitchRad) * std::sin(yawRad),
        distance_ * std::sin(pitchRad),
        distance_ * std::cos(pitchRad) * std::cos(yawRad));

    return glm::lookAt(cameraPos + target_, target_, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 SeismicCameraController::BuildProjectionMatrix(
    float aspect,
    float fovDeg,
    float zNear,
    float zFar) const {
    const float safeAspect = std::max(0.01f, aspect);
    return glm::perspective(glm::radians(fovDeg), safeAspect, zNear, zFar);
}

void SeismicCameraController::Reset() {
    yaw_ = -45.0f;
    pitch_ = 35.0f;
    distance_ = 9.0f;
    target_ = glm::vec3(0.0f);
}

void SeismicCameraController::SetTopView() {
    yaw_ = 0.0f;
    pitch_ = 89.9f;
}

void SeismicCameraController::SetIsometricView() {
    yaw_ = -45.0f;
    pitch_ = 35.0f;
}

void SeismicCameraController::SetFrontView() {
    yaw_ = 0.0f;
    pitch_ = 0.0f;
}

void SeismicCameraController::SetSideView() {
    yaw_ = 90.0f;
    pitch_ = 0.0f;
}

void SeismicCameraController::ApplyPreset(PresetView preset) {
    switch (preset) {
    case PresetView::Isometric:
        SetIsometricView();
        break;
    case PresetView::Top:
        SetTopView();
        break;
    case PresetView::Front:
        SetFrontView();
        break;
    case PresetView::Side:
        SetSideView();
        break;
    case PresetView::Reset:
        Reset();
        break;
    }
}

void SeismicCameraController::FitToBounds(
    const glm::vec3 &minValue,
    const glm::vec3 &maxValue,
    float viewportAspect) {
    if (minValue.x > maxValue.x || minValue.y > maxValue.y || minValue.z > maxValue.z) {
        return;
    }

    target_ = (minValue + maxValue) * 0.5f;
    const glm::vec3 extent = maxValue - minValue;
    const float radius = std::max(0.1f, glm::length(extent) * 0.5f);
    constexpr float verticalFovRad = glm::radians(45.0f);
    const float safeAspect = std::max(0.25f, viewportAspect);
    const float horizontalFovRad = 2.0f * std::atan(std::tan(verticalFovRad * 0.5f) * safeAspect);
    const float fitFov = std::max(0.1f, std::min(verticalFovRad, horizontalFovRad));
    distance_ = std::clamp((radius / std::sin(fitFov * 0.5f)) * 1.5f, kMinCameraDistance, kMaxCameraDistance);
}

void SeismicCameraController::Rotate(float dx, float dy) {
    yaw_ += dx * 0.25f;
    pitch_ += dy * 0.25f;
    pitch_ = std::clamp(pitch_, -89.0f, 89.0f);
}

void SeismicCameraController::Pan(float dx, float dy) {
    const float yawRad = glm::radians(yaw_);
    const float pitchRad = glm::radians(pitch_);
    const glm::vec3 forward = -glm::normalize(glm::vec3(
        std::cos(pitchRad) * std::sin(yawRad),
        std::sin(pitchRad),
        std::cos(pitchRad) * std::cos(yawRad)));

    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));

    const float factor = distance_ * 0.0015f;
    target_ += (-right * dx + up * dy) * factor;
}

void SeismicCameraController::Zoom(float wheelDelta) {
    const float zoomScale = std::pow(0.88f, wheelDelta);
    distance_ = std::clamp(distance_ * zoomScale, kMinCameraDistance, kMaxCameraDistance);
}

void SeismicCameraController::SetPitch(float pitch) {
    pitch_ = std::clamp(pitch, -89.0f, 89.0f);
}

void SeismicCameraController::SetDistance(float dist) {
    distance_ = std::clamp(dist, kMinCameraDistance, kMaxCameraDistance);
}

} // namespace seismic
