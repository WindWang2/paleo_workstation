// 层：视图
#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <vector>

#include "../../domain/seismic/sgyvolume.h"

namespace seismic {

enum class SeismicSliceSlot {
    Inline = 0,
    Crossline = 1,
    Time = 2,
    Line = 3
};

class SeismicSliceRenderer {
public:
    SeismicSliceRenderer() = default;
    ~SeismicSliceRenderer();

    SeismicSliceRenderer(const SeismicSliceRenderer &) = delete;
    SeismicSliceRenderer &operator=(const SeismicSliceRenderer &) = delete;

    bool Initialize(QOpenGLFunctions_3_3_Core *gl);
    void Cleanup(QOpenGLFunctions_3_3_Core *gl);

    bool UpdateSlice(
        QOpenGLFunctions_3_3_Core *gl,
        SeismicSliceSlot slot,
        const SgyVolume &volume,
        SgySliceType type,
        int index,
        const SgySliceImage &image);

    bool UpdateLineSlice(
        QOpenGLFunctions_3_3_Core *gl,
        const SgyVolume &volume,
        const std::vector<glm::ivec2> &pathPoints,
        const SgySliceImage &image);

    void Render(
        QOpenGLFunctions_3_3_Core *gl,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const glm::mat4 &model = glm::mat4(1.0f)) const;

    void SetSlotVisible(SeismicSliceSlot slot, bool value);
    [[nodiscard]] bool IsSlotVisible(SeismicSliceSlot slot) const;
    [[nodiscard]] bool IsSlotReady(SeismicSliceSlot slot) const;
    void ClearSlot(QOpenGLFunctions_3_3_Core *gl, SeismicSliceSlot slot);

    void SetVisible(bool value) { visible_ = value; }
    [[nodiscard]] bool IsVisible() const { return visible_; }
    [[nodiscard]] bool IsInitialized() const { return initialized_; }

    static float HorizontalScale() { return 6.0f; }
    static float HeightScale() { return 4.4f; }

private:
    std::unique_ptr<QOpenGLShaderProgram> program_;
    std::array<GLuint, 4> vaos_{};
    std::array<GLuint, 4> vbos_{};
    std::array<GLuint, 4> ebos_{};
    std::array<GLuint, 4> textures_{};
    std::array<GLsizei, 4> indexCounts_{};
    std::array<bool, 4> slotReady_{};
    std::array<bool, 4> slotVisible_{true, true, true, true};
    std::array<std::vector<unsigned int>, 4> dynamicIndices_;
    bool initialized_ = false;
    bool visible_ = true;
};

} // namespace seismic
