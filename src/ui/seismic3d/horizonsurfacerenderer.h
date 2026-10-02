// 层：视图
#pragma once

#include <QColor>
#include <QString>

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

#include "../../domain/seismic/sgyvolume.h"

namespace seismic {

// D7.3 层位面片（规则 IL×XL 网格）：twtMs → y（按体采样率换算采样位），
// NaN 控制点挖洞（对应三角形不生成）。色彩 = twt 归一 → 色标 LUT（逐顶点）。
struct Seismic3DHorizonSurface
{
    QString name;
    QColor color;                                   // 兜底/描边基色
    bool visible = true;
    int inlineMin = 0, inlineCount = 0, inlineStep = 1;
    int xlineMin = 0, xlineCount = 0, xlineStep = 1;
    std::vector<double> twtMs;                      // inlineCount×xlineCount，NaN=无控制点
};

// 层位面渲染器：每层位一个 VAO/VBO/EBO，顶点带色（色标映射 twt 属性）。
class HorizonSurfaceRenderer
{
public:
    HorizonSurfaceRenderer() = default;
    ~HorizonSurfaceRenderer();

    HorizonSurfaceRenderer(const HorizonSurfaceRenderer &) = delete;
    HorizonSurfaceRenderer &operator=(const HorizonSurfaceRenderer &) = delete;

    bool Initialize(QOpenGLFunctions_3_3_Core *gl);
    void Cleanup(QOpenGLFunctions_3_3_Core *gl);

    // 全量重建（items 由面板从 SeismicHorizonGrid/会话拾取转换）
    bool UpdateHorizons(QOpenGLFunctions_3_3_Core *gl,
                        const SgyVolume &volume,
                        const std::vector<Seismic3DHorizonSurface> &items);
    void SetHorizonVisible(int index, bool visible);
    [[nodiscard]] bool IsHorizonVisible(int index) const;
    [[nodiscard]] int HorizonCount() const { return int(surfaces_.size()); }
    [[nodiscard]] GLsizei TriangleCount() const;

    void Render(QOpenGLFunctions_3_3_Core *gl,
                const glm::mat4 &view,
                const glm::mat4 &projection,
                const glm::mat4 &model = glm::mat4(1.0f)) const;

    void SetVisible(bool value) { visible_ = value; }
    [[nodiscard]] bool IsVisible() const { return visible_; }
    [[nodiscard]] bool IsInitialized() const { return initialized_; }

private:
    struct SurfaceGpu
    {
        std::string name;
        bool visible = true;
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        GLsizei indexCount = 0;
    };

    std::unique_ptr<QOpenGLShaderProgram> program_;
    std::vector<SurfaceGpu> surfaces_;
    bool initialized_ = false;
    bool visible_ = true;
};

} // namespace seismic
