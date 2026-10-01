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

    // D7.1 传递函数（GPU 1D LUT）：enable=true 时切片/堆叠层/任意剖面纹理
    // 改传归一化索引（GL_RG8），着色器按 LUT 取色取不透明度。lutRgba 为
    // 256×4 straight-alpha RGBA；重传仅 256B——TF 修改零取数零重烘焙。
    bool SetTransferFunction(QOpenGLFunctions_3_3_Core *gl,
                             const std::vector<unsigned char> &lutRgba,
                             bool enable);
    [[nodiscard]] bool IsTransferFunctionEnabled() const { return tfEnabled_; }

    // D3.3 切片透明度（0..1；渲染期 uniform，重设纹理无关）
    void SetSliceAlpha(float alpha);
    [[nodiscard]] float sliceAlpha() const { return sliceAlpha_; }

    // D3.1 体渲染（切片堆叠）：n 层水平切片按体积分布渲染，交互期降层数、
    // 静止期精渲（数据提取与层调度在面板层——这里只管几何与纹理）
    static constexpr int kMaxStackLayers = 16;
    bool UpdateStackLayer(QOpenGLFunctions_3_3_Core *gl, int layerIdx,
                          const SgyVolume &volume, int sampleIndex, const SgySliceImage &image);
    void SetStackVisible(bool visible) { stackVisible_ = visible; }
    [[nodiscard]] bool IsStackVisible() const { return stackVisible_; }
    void SetStackLayerVisible(int layerIdx, bool visible);

    void SetVisible(bool value) { visible_ = value; }
    [[nodiscard]] bool IsVisible() const { return visible_; }
    [[nodiscard]] bool IsInitialized() const { return initialized_; }

    static float HorizontalScale() { return 6.0f; }
    static float HeightScale() { return 4.4f; }

private:
    // 纹理上传统一口：TF 模式传 GL_RG8 索引+掩码，否则 RGBA8 预烘焙色。
    // 复用同尺寸纹理走 TexSubImage（避免重新分配）。
    bool UploadSliceTexture(QOpenGLFunctions_3_3_Core *gl, GLuint texture,
                            const SgySliceImage &image);

    std::unique_ptr<QOpenGLShaderProgram> program_;
    std::array<GLuint, 4> vaos_{};
    std::array<GLuint, 4> vbos_{};
    std::array<GLuint, 4> ebos_{};
    std::array<GLuint, 4> textures_{};
    std::array<GLsizei, 4> indexCounts_{};
    std::array<bool, 4> slotReady_{};
    std::array<bool, 4> slotVisible_{true, true, true, true};
    std::array<std::vector<unsigned int>, 4> dynamicIndices_;

    // D3.1 体渲染堆叠层（VAO/VBO/纹理 + 就绪/可见标志）
    std::array<GLuint, kMaxStackLayers> stackVaos_{};
    std::array<GLuint, kMaxStackLayers> stackVbos_{};
    std::array<GLuint, kMaxStackLayers> stackTextures_{};
    std::array<bool, kMaxStackLayers> stackReady_{};
    std::array<bool, kMaxStackLayers> stackVisibleLayer_{};
    bool stackVisible_ = false;

    // D3.3 透明度
    float sliceAlpha_ = 1.0f;

    // D7.1 传递函数
    GLuint tfLutTex_ = 0;
    bool tfEnabled_ = false;

    bool initialized_ = false;
    bool visible_ = true;
};

} // namespace seismic
