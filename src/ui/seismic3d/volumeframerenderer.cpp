// 层：视图
// token 例外：DESIGN 数据符号例外：体框架上地层位置的缺省蓝色数据标记，非面板文字。（tools/ui-token-exceptions.json 精确计数）。
#include "volumeframerenderer.h"
#include "seismic3d_internal.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QDebug>
#include <glm/gtc/type_ptr.hpp>

#include "seismicslicerenderer.h"
#include "../paleotheme.h"

namespace seismic {

namespace {

struct LineVertex {
    glm::vec3 position;
    glm::vec3 color;
};

glm::vec3 FrameInk() {
    const QColor ink = PaleoTheme::tokens().textMuted;
    return {ink.redF(), ink.greenF(), ink.blueF()};
}

void AddLine(
    std::vector<LineVertex> &vertices,
    const glm::vec3 &a,
    const glm::vec3 &b,
    const glm::vec3 &color = FrameInk()) {
    vertices.push_back({a, color});
    vertices.push_back({b, color});
}

std::vector<LineVertex> BuildFrameVertices(const SgyVolume &volume) {
    const float horizontalScale = SeismicSliceRenderer::HorizontalScale();
    const float heightScale = SeismicSliceRenderer::HeightScale();

    const float xMin = Normalize(volume.XlineMin(), volume.XlineMin(), volume.XlineMax(), horizontalScale);
    const float xMax = Normalize(volume.XlineMax(), volume.XlineMin(), volume.XlineMax(), horizontalScale);
    const float zMin = Normalize(volume.InlineMin(), volume.InlineMin(), volume.InlineMax(), horizontalScale);
    const float zMax = Normalize(volume.InlineMax(), volume.InlineMin(), volume.InlineMax(), horizontalScale);
    const float yTop = heightScale * 0.5f;
    const float yBottom = -heightScale * 0.5f;

    const glm::vec3 p000(xMin, yBottom, zMin);
    const glm::vec3 p100(xMax, yBottom, zMin);
    const glm::vec3 p110(xMax, yBottom, zMax);
    const glm::vec3 p010(xMin, yBottom, zMax);
    const glm::vec3 p001(xMin, yTop, zMin);
    const glm::vec3 p101(xMax, yTop, zMin);
    const glm::vec3 p111(xMax, yTop, zMax);
    const glm::vec3 p011(xMin, yTop, zMax);

    std::vector<LineVertex> vertices;
    vertices.reserve(24);
    // Bottom rectangle
    AddLine(vertices, p000, p100);
    AddLine(vertices, p100, p110);
    AddLine(vertices, p110, p010);
    AddLine(vertices, p010, p000);
    // Top rectangle
    AddLine(vertices, p001, p101);
    AddLine(vertices, p101, p111);
    AddLine(vertices, p111, p011);
    AddLine(vertices, p011, p001);
    // Vertical edges
    AddLine(vertices, p000, p001);
    AddLine(vertices, p100, p101);
    AddLine(vertices, p110, p111);
    AddLine(vertices, p010, p011);

    return vertices;
}

} // namespace

VolumeFrameRenderer::~VolumeFrameRenderer() {
}

void VolumeFrameRenderer::Cleanup(QOpenGLFunctions_3_3_Core *gl) {
    if (!gl) return;

    if (vbo_) {
        gl->glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_) {
        gl->glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    program_.reset();

    initialized_ = false;
    vertexCount_ = 0;
    frameVertexCount_ = 0;
}

bool VolumeFrameRenderer::Initialize(QOpenGLFunctions_3_3_Core *gl) {
    if (!gl) return false;
    Cleanup(gl);

    program_ = std::make_unique<QOpenGLShaderProgram>();
    if (!program_->addShaderFromSourceFile(QOpenGLShader::Vertex, QStringLiteral(":/paleo/shaders/seismic/axis.vert.glsl"))) {
        qWarning() << "VolumeFrameRenderer: failed to compile axis vertex shader:" << program_->log();
        return false;
    }
    if (!program_->addShaderFromSourceFile(QOpenGLShader::Fragment, QStringLiteral(":/paleo/shaders/seismic/axis.frag.glsl"))) {
        qWarning() << "VolumeFrameRenderer: failed to compile axis fragment shader:" << program_->log();
        return false;
    }
    if (!program_->link()) {
        qWarning() << "VolumeFrameRenderer: failed to link axis shader:" << program_->log();
        return false;
    }

    gl->glGenVertexArrays(1, &vao_);
    gl->glGenBuffers(1, &vbo_);

    gl->glBindVertexArray(vao_);
    gl->glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl->glBufferData(GL_ARRAY_BUFFER, sizeof(LineVertex) * 64, nullptr, GL_DYNAMIC_DRAW);

    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), reinterpret_cast<void*>(offsetof(LineVertex, position)));
    gl->glEnableVertexAttribArray(0);

    gl->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), reinterpret_cast<void*>(offsetof(LineVertex, color)));
    gl->glEnableVertexAttribArray(1);

    gl->glBindVertexArray(0);

    initialized_ = true;
    return true;
}

void VolumeFrameRenderer::UpdateFromVolume(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &volume) {
    if (!gl || !initialized_ || !volume.IsLoaded()) {
        return;
    }
    Upload(gl, volume);
}

void VolumeFrameRenderer::UpdateLineSection(
    QOpenGLFunctions_3_3_Core *gl,
    const SgyVolume &volume,
    const std::vector<glm::ivec2> &drawPathPoints) {
    if (!gl || !initialized_ || !volume.IsLoaded() || drawPathPoints.size() < 2) {
        return;
    }
    linePath_ = drawPathPoints;
    Upload(gl, volume);
}

void VolumeFrameRenderer::ClearLineSection() {
    linePath_.clear();
    vertexCount_ = frameVertexCount_; // 旧语义：立即回退到仅包围盒计数
}

// ---- D3.4 井位标记 ----
void VolumeFrameRenderer::UpdateWells(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &volume,
                                      const std::vector<Seismic3DWell> &wells) {
    wells_ = wells;
    if (gl && initialized_ && volume.IsLoaded())
        Upload(gl, volume);
}

void VolumeFrameRenderer::ClearWells(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &volume) {
    wells_.clear();
    if (gl && initialized_ && volume.IsLoaded())
        Upload(gl, volume);
}

// ---- D3.12 多体叠加轮廓 ----
void VolumeFrameRenderer::SetSecondaryVolume(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &primary,
                                             std::shared_ptr<const SgyVolume> secondary) {
    secondary_ = std::move(secondary);
    if (gl && initialized_ && primary.IsLoaded())
        Upload(gl, primary);
}

// 统一重建：frame(+line path)+井+第二体 → 一次上传
void VolumeFrameRenderer::Upload(QOpenGLFunctions_3_3_Core *gl, const SgyVolume &volume) {
    const float horizontalScale = SeismicSliceRenderer::HorizontalScale();
    const float heightScale = SeismicSliceRenderer::HeightScale();
    const float yTop = heightScale * 0.5f + 0.003f;

    std::vector<LineVertex> vertices = BuildFrameVertices(volume);
    frameVertexCount_ = static_cast<GLsizei>(vertices.size());

    // 任意线路径高亮（顶面橙色路径）
    if (linePath_.size() >= 2) {
        const glm::vec3 sectionColor(0.95f, 0.60f, 0.00f);
        for (size_t i = 1; i < linePath_.size(); ++i) {
            const float ax = Normalize(linePath_[i - 1].y, volume.XlineMin(), volume.XlineMax(), horizontalScale);
            const float az = Normalize(linePath_[i - 1].x, volume.InlineMin(), volume.InlineMax(), horizontalScale);
            const float bx = Normalize(linePath_[i].y, volume.XlineMin(), volume.XlineMax(), horizontalScale);
            const float bz = Normalize(linePath_[i].x, volume.InlineMin(), volume.InlineMax(), horizontalScale);
            AddLine(vertices, glm::vec3(ax, yTop, az), glm::vec3(bx, yTop, bz), sectionColor);
        }
    }

    // D3.4 井轨迹（垂直线）+ 标志层十字标；D7.3 斜井折线轨迹
    for (const Seismic3DWell &well : wells_) {
        const float x = Normalize(well.xlineNo, volume.XlineMin(), volume.XlineMax(), horizontalScale);
        const float z = Normalize(well.inlineNo, volume.InlineMin(), volume.InlineMax(), horizontalScale);
        const float yTopWell = heightScale * 0.5f;
        const float yBottomWell = heightScale * 0.5f - std::clamp(well.bottomFrac, 0.02f, 1.0f) * heightScale;
        // 白色光晕 + 蓝主色（与 2D 剖面井筒一致）
        AddLine(vertices, glm::vec3(x, yTopWell, z), glm::vec3(x, yBottomWell, z),
                glm::vec3(0.95f, 0.95f, 1.0f));
        AddLine(vertices, glm::vec3(x, yTopWell, z), glm::vec3(x, yBottomWell, z),
                glm::vec3(0.11f, 0.45f, 0.82f));
        // D7.3 轨迹折线（白色光晕 + 蓝主色双描；轨迹深度用 sampleFrac）
        if (well.trajectory.size() >= 2) {
            const auto trajY = [&](float sampleFrac) {
                return heightScale * 0.5f - std::clamp(sampleFrac, 0.0f, 1.0f) * heightScale;
            };
            for (size_t i = 1; i < well.trajectory.size(); ++i) {
                const auto &a = well.trajectory[i - 1];
                const auto &b = well.trajectory[i];
                const glm::vec3 pa(NormalizeF(a.xlineNo, volume.XlineMin(), volume.XlineMax(), horizontalScale),
                                   trajY(a.sampleFrac),
                                   NormalizeF(a.inlineNo, volume.InlineMin(), volume.InlineMax(), horizontalScale));
                const glm::vec3 pb(NormalizeF(b.xlineNo, volume.XlineMin(), volume.XlineMax(), horizontalScale),
                                   trajY(b.sampleFrac),
                                   NormalizeF(b.inlineNo, volume.InlineMin(), volume.InlineMax(), horizontalScale));
                AddLine(vertices, pa, pb, glm::vec3(0.95f, 0.95f, 1.0f));
                AddLine(vertices, pa, pb, glm::vec3(0.11f, 0.45f, 0.82f));
            }
        }
        // 标志层：短水平十字
        for (const Seismic3DWellTop &top : well.tops) {
            const float y = heightScale * 0.5f - std::clamp(top.sampleFrac, 0.0f, 1.0f) * heightScale;
            const QColor c = top.color.isValid() ? top.color : QColor(0x1B73D0);
            const glm::vec3 col(c.redF(), c.greenF(), c.blueF());
            AddLine(vertices, glm::vec3(x - 0.10f, y, z), glm::vec3(x + 0.10f, y, z), col);
            AddLine(vertices, glm::vec3(x, y, z - 0.10f), glm::vec3(x, y, z + 0.10f), col);
        }
    }

    // D3.12 第二工区轮廓（青色虚线观感——线段断续）
    if (secondary_ && secondary_->IsLoaded()) {
        const glm::vec3 secondaryColor(0.20f, 0.80f, 0.85f);
        const float xMin = Normalize(secondary_->XlineMin(), volume.XlineMin(), volume.XlineMax(), horizontalScale);
        const float xMax = Normalize(secondary_->XlineMax(), volume.XlineMin(), volume.XlineMax(), horizontalScale);
        const float zMin = Normalize(secondary_->InlineMin(), volume.InlineMin(), volume.InlineMax(), horizontalScale);
        const float zMax = Normalize(secondary_->InlineMax(), volume.InlineMin(), volume.InlineMax(), horizontalScale);
        const float yT = heightScale * 0.5f + 0.006f;
        AddLine(vertices, glm::vec3(xMin, yT, zMin), glm::vec3(xMax, yT, zMin), secondaryColor);
        AddLine(vertices, glm::vec3(xMax, yT, zMin), glm::vec3(xMax, yT, zMax), secondaryColor);
        AddLine(vertices, glm::vec3(xMax, yT, zMax), glm::vec3(xMin, yT, zMax), secondaryColor);
        AddLine(vertices, glm::vec3(xMin, yT, zMax), glm::vec3(xMin, yT, zMin), secondaryColor);
    }

    vertexCount_ = static_cast<GLsizei>(vertices.size());
    gl->glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(LineVertex) * vertices.size()), vertices.data(), GL_DYNAMIC_DRAW);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void VolumeFrameRenderer::Render(
    QOpenGLFunctions_3_3_Core *gl,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const glm::mat4 &model) const {
    if (!gl || !initialized_ || !visible_ || vertexCount_ == 0 || !program_) {
        return;
    }

    program_->bind();
    const GLuint progId = program_->programId();

    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "model"), 1, GL_FALSE, glm::value_ptr(model));
    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "view"), 1, GL_FALSE, glm::value_ptr(view));
    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "projection"), 1, GL_FALSE, glm::value_ptr(projection));

    gl->glDisable(GL_CULL_FACE);
    gl->glBindVertexArray(vao_);
    gl->glLineWidth(1.0f);
    // Bounding frame is UI chrome; geological overlay vertex colours stay unchanged.
    const auto ink = FrameInk();
    gl->glDisableVertexAttribArray(1);
    program_->setAttributeValue(1, ink.r, ink.g, ink.b);
    gl->glDrawArrays(GL_LINES, 0, frameVertexCount_);
    gl->glEnableVertexAttribArray(1);
    gl->glDrawArrays(GL_LINES, frameVertexCount_, vertexCount_ - frameVertexCount_);
    gl->glBindVertexArray(0);
    gl->glEnable(GL_CULL_FACE);

    program_->release();
}

} // namespace seismic
