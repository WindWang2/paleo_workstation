// 层：视图
#pragma once

#include <QColor>
#include <QString>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "../../domain/seismic/sgyvolume.h"

namespace seismic {

// D3.4 三维井位：测线格坐标 + 井底/标志层位置（以体积比例 0=顶 1=底）
struct Seismic3DWellTop {
    QString name;
    float sampleFrac = 0.5f;
    QColor color;
};

// D7.3 井轨迹点：连续测网坐标（斜井/侧钻逐点）+ 归一采样位（0=顶 1=底）。
// 调用方负责 XY→(inline,xline) 与时深→sampleFrac 换算（SurveyGridGeometry/体采样率）。
struct Seismic3DWellTrajPoint {
    float inlineNo = 0.0f;
    float xlineNo = 0.0f;
    float sampleFrac = 0.0f;
};

struct Seismic3DWell {
    QString name;
    int inlineNo = 0;
    int xlineNo = 0;
    float bottomFrac = 1.0f;  // 井底（0=体积顶，1=体积底）
    std::vector<Seismic3DWellTop> tops;
    // D7.3 轨迹（≥2 点走折线；空 = D3.4 垂直井简化）
    std::vector<Seismic3DWellTrajPoint> trajectory;
};

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

    // D3.4 井位标记：井轨迹（垂直线）+ 标志层点（十字标）。
    // 调用者负责把井口 XY 换算到 (inlineNo, xlineNo)（时深→sampleFrac 同理）。
    void UpdateWells(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &volume,
                     const std::vector<Seismic3DWell> &wells);
    void ClearWells(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &volume);

    // D3.12 多体叠加：第二工区包围盒轮廓（橙色，按主工区测线坐标系归一）。
    void SetSecondaryVolume(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &primary,
                            std::shared_ptr<const SgyVolume> secondary);
    [[nodiscard]] bool HasSecondaryVolume() const { return secondary_ != nullptr; }

    void Render(
        QOpenGLFunctions_3_3_Core *gl,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const glm::mat4 &model = glm::mat4(1.0f)) const;

    void SetVisible(bool value) { visible_ = value; }
    [[nodiscard]] bool IsVisible() const { return visible_; }
    [[nodiscard]] bool IsInitialized() const { return initialized_; }
    [[nodiscard]] GLsizei VertexCount() const { return vertexCount_; }

private:
    void Upload(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &volume);

    std::unique_ptr<QOpenGLShaderProgram> program_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLsizei vertexCount_ = 0;
    GLsizei frameVertexCount_ = 0;
    bool initialized_ = false;
    bool visible_ = true;

    std::vector<glm::ivec2> linePath_;             // 任意线路径（重绘合并用）
    std::vector<Seismic3DWell> wells_;             // D3.4
    std::shared_ptr<const SgyVolume> secondary_;   // D3.12
};

} // namespace seismic
