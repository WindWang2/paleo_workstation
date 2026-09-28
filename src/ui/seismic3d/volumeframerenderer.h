// 层：视图
#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

#include "../../domain/seismic/sgyvolume.h"

namespace seismic {

class VolumeFrameRenderer {
public:
    VolumeFrameRenderer() = default;
    ~VolumeFrameRenderer();

    VolumeFrameRenderer(const VolumeFrameRenderer &) = delete;
    VolumeFrameRenderer &operator=(const VolumeFrameRenderer &) = delete;

    bool Initialize(QOpenGLFunctions_3_3_Core *gl);
    void Cleanup(QOpenGLFunctions_3_3_Core *gl);

    void UpdateFromVolume(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &volume);
    void UpdateLineSection(
        QOpenGLFunctions_3_3_Core *gl,
        const SgyVolume &volume,
        const std::vector<glm::ivec2> &drawPathPoints);
    void ClearLineSection();

    void Render(
        QOpenGLFunctions_3_3_Core *gl,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const glm::mat4 &model = glm::mat4(1.0f)) const;

    void SetVisible(bool value) { visible_ = value; }
    [[nodiscard]] bool IsVisible() const { return visible_; }
    [[nodiscard]] bool IsInitialized() const { return initialized_; }
    [[nodiscard]] int VertexCount() const { return vertexCount_; }

private:
    std::unique_ptr<QOpenGLShaderProgram> program_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLsizei vertexCount_ = 0;
    GLsizei frameVertexCount_ = 0;
    bool initialized_ = false;
    bool visible_ = true;
};

} // namespace seismic
