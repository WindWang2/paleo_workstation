// 层：视图
#include "horizonsurfacerenderer.h"
#include "seismic3d_internal.h"

#include "seismic3dcolormap.h"
#include "seismicslicerenderer.h"

#include <QDebug>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace seismic {

namespace {

struct SurfaceVertex {
    glm::vec3 position;
    glm::vec3 color;
};

} // namespace

HorizonSurfaceRenderer::~HorizonSurfaceRenderer()
{
}

void HorizonSurfaceRenderer::Cleanup(QOpenGLFunctions_3_3_Core *gl)
{
    if (!gl) return;
    for (SurfaceGpu &s : surfaces_) {
        if (s.ebo) gl->glDeleteBuffers(1, &s.ebo);
        if (s.vbo) gl->glDeleteBuffers(1, &s.vbo);
        if (s.vao) gl->glDeleteVertexArrays(1, &s.vao);
    }
    surfaces_.clear();
    program_.reset();
    initialized_ = false;
}

bool HorizonSurfaceRenderer::Initialize(QOpenGLFunctions_3_3_Core *gl)
{
    if (!gl) return false;
    Cleanup(gl);

    program_ = std::make_unique<QOpenGLShaderProgram>();
    static const char *kVertSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
out vec3 vColor;
void main() {
    vColor = color;
    gl_Position = projection * view * model * vec4(position, 1.0);
}
)GLSL";
    static const char *kFragSrc = R"GLSL(
#version 330 core
in vec3 vColor;
uniform float uAlpha;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor, uAlpha);
}
)GLSL";
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertSrc) ||
        !program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc) ||
        !program_->link()) {
        qWarning() << "HorizonSurfaceRenderer: shader compile/link failed:" << program_->log();
        return false;
    }

    initialized_ = true;
    return true;
}

bool HorizonSurfaceRenderer::UpdateHorizons(
    QOpenGLFunctions_3_3_Core *gl,
    const SgyVolume &volume,
    const std::vector<Seismic3DHorizonSurface> &items)
{
    if (!gl || !initialized_ || !volume.IsLoaded())
        return false;

    for (SurfaceGpu &s : surfaces_) {
        if (s.ebo) gl->glDeleteBuffers(1, &s.ebo);
        if (s.vbo) gl->glDeleteBuffers(1, &s.vbo);
        if (s.vao) gl->glDeleteVertexArrays(1, &s.vao);
    }
    surfaces_.clear();
    surfaces_.reserve(items.size());

    const float hScale = SeismicSliceRenderer::HorizontalScale();
    const float vScale = SeismicSliceRenderer::HeightScale();
    const double msPerSample = volume.SampleIntervalUs() / 1000.0;
    const double maxMs = std::max(1.0, msPerSample * volume.SampleMax());

    for (const Seismic3DHorizonSurface &item : items) {
        SurfaceGpu gpu;
        gpu.name = item.name.toStdString();
        gpu.visible = item.visible;

        const int ni = std::max(0, item.inlineCount);
        const int nx = std::max(0, item.xlineCount);
        if (ni < 2 || nx < 2 ||
            item.twtMs.size() != static_cast<std::size_t>(ni) * static_cast<std::size_t>(nx)) {
            surfaces_.push_back(gpu); // 空层位占位（计数一致，渲染跳过）
            continue;
        }

        // twt 值域 → 色标 LUT（彩虹谱——数据符号，逐层位独立归一）
        double lo = std::numeric_limits<double>::max();
        double hi = std::numeric_limits<double>::lowest();
        for (double v : item.twtMs) {
            if (std::isfinite(v)) {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        if (!(hi > lo))
            hi = lo + 1.0; // 平层：单值映射到 LUT 中段
        const auto lut = Seismic3DColorMap::preset(QStringLiteral("彩虹谱")).buildLut();

        std::vector<SurfaceVertex> vertices;
        vertices.reserve(static_cast<std::size_t>(ni) * static_cast<std::size_t>(nx));
        std::vector<unsigned int> indices;
        indices.reserve(static_cast<std::size_t>((ni - 1) * (nx - 1)) * 6);
        const auto nodeValue = [&](int i, int x) -> double {
            return item.twtMs[static_cast<std::size_t>(i) * static_cast<std::size_t>(nx) +
                              static_cast<std::size_t>(x)];
        };
        for (int i = 0; i < ni; ++i) {
            for (int x = 0; x < nx; ++x) {
                const double twt = nodeValue(i, x);
                if (!std::isfinite(twt))
                    continue;
                const float inlineNo =
                    static_cast<float>(item.inlineMin + i * item.inlineStep);
                const float xlineNo =
                    static_cast<float>(item.xlineMin + x * item.xlineStep);
                const float sampleFrac =
                    static_cast<float>(std::clamp(twt / maxMs, 0.0, 1.0));
                const int lutIdx = static_cast<int>(
                    std::clamp((twt - lo) / (hi - lo), 0.0, 1.0) * 255.0);
                const QRgb c = lut[static_cast<std::size_t>(lutIdx)];
                SurfaceVertex v;
                v.position = glm::vec3(
                    NormalizeF(xlineNo, volume.XlineMin(), volume.XlineMax(), hScale),
                    vScale * 0.5f - sampleFrac * vScale,
                    NormalizeF(inlineNo, volume.InlineMin(), volume.InlineMax(), hScale));
                v.color = glm::vec3(qRed(c) / 255.0f, qGreen(c) / 255.0f, qBlue(c) / 255.0f);
                vertices.push_back(v);
            }
        }
        // 四角全在的格子出双三角（NaN 控制点自然挖洞）；有限点连续编号
        std::vector<int> map(static_cast<std::size_t>(ni) * static_cast<std::size_t>(nx), -1);
        {
            int next = 0;
            for (std::size_t k = 0; k < map.size(); ++k)
                if (std::isfinite(item.twtMs[k]))
                    map[k] = next++;
        }
        for (int i = 0; i + 1 < ni; ++i) {
            for (int x = 0; x + 1 < nx; ++x) {
                const int a = map[static_cast<std::size_t>(i) * nx + x];
                const int b = map[static_cast<std::size_t>(i) * nx + (x + 1)];
                const int c = map[static_cast<std::size_t>(i + 1) * nx + (x + 1)];
                const int d = map[static_cast<std::size_t>(i + 1) * nx + x];
                if (a < 0 || b < 0 || c < 0 || d < 0)
                    continue;
                indices.push_back(static_cast<unsigned int>(a));
                indices.push_back(static_cast<unsigned int>(d));
                indices.push_back(static_cast<unsigned int>(b));
                indices.push_back(static_cast<unsigned int>(b));
                indices.push_back(static_cast<unsigned int>(d));
                indices.push_back(static_cast<unsigned int>(c));
            }
        }

        gl->glGenVertexArrays(1, &gpu.vao);
        gl->glGenBuffers(1, &gpu.vbo);
        gl->glGenBuffers(1, &gpu.ebo);
        gl->glBindVertexArray(gpu.vao);
        gl->glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        gl->glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(sizeof(SurfaceVertex) * vertices.size()),
                         vertices.data(), GL_STATIC_DRAW);
        gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
        gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(sizeof(unsigned int) * indices.size()),
                         indices.data(), GL_STATIC_DRAW);
        gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                                  reinterpret_cast<void *>(offsetof(SurfaceVertex, position)));
        gl->glEnableVertexAttribArray(0);
        gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(SurfaceVertex),
                                  reinterpret_cast<void *>(offsetof(SurfaceVertex, color)));
        gl->glEnableVertexAttribArray(1);
        gl->glBindVertexArray(0);
        gpu.indexCount = static_cast<GLsizei>(indices.size());
        surfaces_.push_back(gpu);
    }
    return true;
}

void HorizonSurfaceRenderer::SetHorizonVisible(int index, bool visible)
{
    if (index >= 0 && index < int(surfaces_.size()))
        surfaces_[static_cast<std::size_t>(index)].visible = visible;
}

bool HorizonSurfaceRenderer::IsHorizonVisible(int index) const
{
    if (index < 0 || index >= int(surfaces_.size()))
        return false;
    return surfaces_[static_cast<std::size_t>(index)].visible;
}

GLsizei HorizonSurfaceRenderer::TriangleCount() const
{
    GLsizei total = 0;
    for (const SurfaceGpu &s : surfaces_)
        total += s.indexCount / 3;
    return total;
}

void HorizonSurfaceRenderer::Render(
    QOpenGLFunctions_3_3_Core *gl,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const glm::mat4 &model) const
{
    if (!gl || !initialized_ || !visible_ || !program_)
        return;

    program_->bind();
    const GLuint progId = program_->programId();
    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "model"), 1, GL_FALSE, glm::value_ptr(model));
    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "view"), 1, GL_FALSE, glm::value_ptr(view));
    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
    gl->glUniform1f(gl->glGetUniformLocation(progId, "uAlpha"), 0.88f);

    gl->glDisable(GL_CULL_FACE);
    gl->glEnable(GL_BLEND);
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDepthMask(GL_FALSE); // 半透明面不写深度（与堆叠层同策略）
    for (const SurfaceGpu &s : surfaces_) {
        if (!s.visible || s.indexCount == 0)
            continue;
        gl->glBindVertexArray(s.vao);
        gl->glDrawElements(GL_TRIANGLES, s.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    gl->glBindVertexArray(0);
    gl->glDepthMask(GL_TRUE);
    gl->glDisable(GL_BLEND);
    gl->glEnable(GL_CULL_FACE);
    program_->release();
}

} // namespace seismic
