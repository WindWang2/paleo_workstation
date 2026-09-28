// 层：视图
#include "volumeframerenderer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QDebug>
#include <glm/gtc/type_ptr.hpp>

#include "seismicslicerenderer.h"

namespace seismic {

namespace {

struct LineVertex {
    glm::vec3 position;
    glm::vec3 color;
};

float Normalize(int value, int minValue, int maxValue, float scale) {
    const float range = static_cast<float>(std::max(1, maxValue - minValue));
    return ((static_cast<float>(value - minValue) / range) - 0.5f) * scale;
}

void AddLine(
    std::vector<LineVertex> &vertices,
    const glm::vec3 &a,
    const glm::vec3 &b,
    const glm::vec3 &color = glm::vec3(0.60f, 0.68f, 0.78f)) {
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

    const std::vector<LineVertex> vertices = BuildFrameVertices(volume);
    frameVertexCount_ = static_cast<GLsizei>(vertices.size());
    vertexCount_ = frameVertexCount_;

    gl->glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(LineVertex) * vertices.size()), vertices.data(), GL_DYNAMIC_DRAW);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void VolumeFrameRenderer::UpdateLineSection(
    QOpenGLFunctions_3_3_Core *gl,
    const SgyVolume &volume,
    const std::vector<glm::ivec2> &drawPathPoints) {
    if (!gl || !initialized_ || !volume.IsLoaded() || drawPathPoints.size() < 2) {
        return;
    }

    const float horizontalScale = SeismicSliceRenderer::HorizontalScale();
    const float heightScale = SeismicSliceRenderer::HeightScale();
    const float yTop = heightScale * 0.5f + 0.003f;
    const glm::vec3 sectionColor(0.95f, 0.60f, 0.00f);

    std::vector<LineVertex> vertices = BuildFrameVertices(volume);
    frameVertexCount_ = static_cast<GLsizei>(vertices.size());

    for (size_t i = 1; i < drawPathPoints.size(); ++i) {
        const float ax = Normalize(drawPathPoints[i - 1].y, volume.XlineMin(), volume.XlineMax(), horizontalScale);
        const float az = Normalize(drawPathPoints[i - 1].x, volume.InlineMin(), volume.InlineMax(), horizontalScale);
        const float bx = Normalize(drawPathPoints[i].y, volume.XlineMin(), volume.XlineMax(), horizontalScale);
        const float bz = Normalize(drawPathPoints[i].x, volume.InlineMin(), volume.InlineMax(), horizontalScale);
        AddLine(vertices, glm::vec3(ax, yTop, az), glm::vec3(bx, yTop, bz), sectionColor);
    }

    vertexCount_ = static_cast<GLsizei>(vertices.size());

    gl->glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(LineVertex) * vertices.size()), vertices.data(), GL_DYNAMIC_DRAW);
    gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void VolumeFrameRenderer::ClearLineSection() {
    vertexCount_ = frameVertexCount_;
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
    gl->glDrawArrays(GL_LINES, 0, vertexCount_);
    gl->glBindVertexArray(0);
    gl->glEnable(GL_CULL_FACE);

    program_->release();
}

} // namespace seismic
