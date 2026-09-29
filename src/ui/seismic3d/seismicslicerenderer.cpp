// 层：视图
#include "seismicslicerenderer.h"

#include <algorithm>
#include <cmath>

#include <QDebug>
#include <glm/gtc/type_ptr.hpp>

namespace seismic {

namespace {

struct SliceVertex {
    glm::vec3 position;
    glm::vec2 uv;
};

float Normalize(int value, int minValue, int maxValue, float scale) {
    const float range = static_cast<float>(std::max(1, maxValue - minValue));
    return ((static_cast<float>(value - minValue) / range) - 0.5f) * scale;
}

std::array<SliceVertex, 4> BuildSliceVertices(const SgyVolume &volume, SgySliceType type, int index) {
    const float horizontalScale = SeismicSliceRenderer::HorizontalScale();
    const float heightScale = SeismicSliceRenderer::HeightScale();

    const float xMin = Normalize(volume.XlineMin(), volume.XlineMin(), volume.XlineMax(), horizontalScale);
    const float xMax = Normalize(volume.XlineMax(), volume.XlineMin(), volume.XlineMax(), horizontalScale);
    const float zMin = Normalize(volume.InlineMin(), volume.InlineMin(), volume.InlineMax(), horizontalScale);
    const float zMax = Normalize(volume.InlineMax(), volume.InlineMin(), volume.InlineMax(), horizontalScale);

    const float yTop = -((0.0f / std::max(1.0f, static_cast<float>(volume.SampleMax()))) - 0.5f) * heightScale;
    const float yBottom = -((static_cast<float>(volume.SampleMax()) / std::max(1.0f, static_cast<float>(volume.SampleMax()))) - 0.5f) * heightScale;

    if (type == SgySliceType::Inline) {
        const float z = Normalize(index, volume.InlineMin(), volume.InlineMax(), horizontalScale);
        return {{
            {{xMin, yBottom, z}, {0.0f, 0.0f}},
            {{xMax, yBottom, z}, {1.0f, 0.0f}},
            {{xMax, yTop, z}, {1.0f, 1.0f}},
            {{xMin, yTop, z}, {0.0f, 1.0f}},
        }};
    }

    if (type == SgySliceType::Xline) {
        const float x = Normalize(index, volume.XlineMin(), volume.XlineMax(), horizontalScale);
        return {{
            {{x, yBottom, zMin}, {0.0f, 0.0f}},
            {{x, yBottom, zMax}, {1.0f, 0.0f}},
            {{x, yTop, zMax}, {1.0f, 1.0f}},
            {{x, yTop, zMin}, {0.0f, 1.0f}},
        }};
    }

    // Time slice (Z)
    const float y = -((static_cast<float>(index) / std::max(1.0f, static_cast<float>(volume.SampleMax()))) - 0.5f) * heightScale;
    return {{
        {{xMin, y, zMin}, {0.0f, 1.0f}},
        {{xMax, y, zMin}, {1.0f, 1.0f}},
        {{xMax, y, zMax}, {1.0f, 0.0f}},
        {{xMin, y, zMax}, {0.0f, 0.0f}},
    }};
}

} // namespace

SeismicSliceRenderer::~SeismicSliceRenderer() {
    // Note: GL resources should ideally be cleaned up via Cleanup(gl)
    // while the OpenGL context is current.
}

void SeismicSliceRenderer::Cleanup(QOpenGLFunctions_3_3_Core *gl) {
    if (!gl) return;

    for (GLuint &tex : textures_) {
        if (tex) {
            gl->glDeleteTextures(1, &tex);
            tex = 0;
        }
    }
    for (GLuint &ebo : ebos_) {
        if (ebo) {
            gl->glDeleteBuffers(1, &ebo);
            ebo = 0;
        }
    }
    for (GLuint &vbo : vbos_) {
        if (vbo) {
            gl->glDeleteBuffers(1, &vbo);
            vbo = 0;
        }
    }
    for (GLuint &vao : vaos_) {
        if (vao) {
            gl->glDeleteVertexArrays(1, &vao);
            vao = 0;
        }
    }
    gl->glDeleteVertexArrays(kMaxStackLayers, stackVaos_.data());
    gl->glDeleteBuffers(kMaxStackLayers, stackVbos_.data());
    gl->glDeleteTextures(kMaxStackLayers, stackTextures_.data());
    stackVaos_.fill(0);
    stackVbos_.fill(0);
    stackTextures_.fill(0);
    stackReady_.fill(false);
    program_.reset();

    indexCounts_.fill(0);
    slotReady_.fill(false);
    slotVisible_.fill(true);
    for (auto &indices : dynamicIndices_) {
        indices.clear();
    }
    initialized_ = false;
}

bool SeismicSliceRenderer::Initialize(QOpenGLFunctions_3_3_Core *gl) {
    if (!gl) return false;
    Cleanup(gl);

    program_ = std::make_unique<QOpenGLShaderProgram>();
    // D3.3：透明度/裁剪着色器内联（qrc 资源属共享只读，新 uniform 走源码）
    static const char *kVertSrc = R"GLSL(
#version 330 core
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
out vec2 vUv;
void main() {
    vUv = uv;
    gl_Position = projection * view * model * vec4(position, 1.0);
}
)GLSL";
    static const char *kFragSrc = R"GLSL(
#version 330 core
in vec2 vUv;
uniform sampler2D sliceTexture;
uniform float uAlpha;      // D3.3 切片透明度
out vec4 fragColor;
void main() {
    vec4 tex = texture(sliceTexture, vUv);
    if (tex.a < 0.02) discard;   // D3.3 值域裁剪（CPU 侧把带外像素 alpha 置 0）
    fragColor = vec4(tex.rgb, tex.a * uAlpha);
}
)GLSL";
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertSrc)) {
        qWarning() << "SeismicSliceRenderer: failed to compile vertex shader:" << program_->log();
        return false;
    }
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc)) {
        qWarning() << "SeismicSliceRenderer: failed to compile fragment shader:" << program_->log();
        return false;
    }
    if (!program_->link()) {
        qWarning() << "SeismicSliceRenderer: failed to link shader program:" << program_->log();
        return false;
    }

    gl->glGenVertexArrays(static_cast<GLsizei>(vaos_.size()), vaos_.data());
    gl->glGenBuffers(static_cast<GLsizei>(vbos_.size()), vbos_.data());
    gl->glGenBuffers(static_cast<GLsizei>(ebos_.size()), ebos_.data());
    gl->glGenTextures(static_cast<GLsizei>(textures_.size()), textures_.data());

    const unsigned int indices[] = {0, 1, 2, 0, 2, 3};
    for (size_t slot = 0; slot < vaos_.size(); ++slot) {
        gl->glBindVertexArray(vaos_[slot]);

        gl->glBindBuffer(GL_ARRAY_BUFFER, vbos_[slot]);
        gl->glBufferData(GL_ARRAY_BUFFER, sizeof(SliceVertex) * 4, nullptr, GL_DYNAMIC_DRAW);

        gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebos_[slot]);
        gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SliceVertex), reinterpret_cast<void*>(offsetof(SliceVertex, position)));
        gl->glEnableVertexAttribArray(0);

        gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SliceVertex), reinterpret_cast<void*>(offsetof(SliceVertex, uv)));
        gl->glEnableVertexAttribArray(1);

        gl->glBindTexture(GL_TEXTURE_2D, textures_[slot]);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        indexCounts_[slot] = 6;
    }

    // D3.1 体渲染堆叠层资源
    gl->glGenVertexArrays(kMaxStackLayers, stackVaos_.data());
    gl->glGenBuffers(kMaxStackLayers, stackVbos_.data());
    gl->glGenTextures(kMaxStackLayers, stackTextures_.data());
    for (int layer = 0; layer < kMaxStackLayers; ++layer) {
        gl->glBindVertexArray(stackVaos_[static_cast<std::size_t>(layer)]);
        gl->glBindBuffer(GL_ARRAY_BUFFER, stackVbos_[static_cast<std::size_t>(layer)]);
        gl->glBufferData(GL_ARRAY_BUFFER, sizeof(SliceVertex) * 4, nullptr, GL_DYNAMIC_DRAW);
        // 每层固定 4 顶点两三角：element buffer 复用 ebos_[Time]
        gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebos_[static_cast<size_t>(SeismicSliceSlot::Time)]);
        gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SliceVertex), reinterpret_cast<void *>(offsetof(SliceVertex, position)));
        gl->glEnableVertexAttribArray(0);
        gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SliceVertex), reinterpret_cast<void *>(offsetof(SliceVertex, uv)));
        gl->glEnableVertexAttribArray(1);
        gl->glBindTexture(GL_TEXTURE_2D, stackTextures_[static_cast<std::size_t>(layer)]);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        stackReady_[static_cast<std::size_t>(layer)] = false;
        stackVisibleLayer_[static_cast<std::size_t>(layer)] = true;
    }

    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glBindVertexArray(0);

    initialized_ = true;
    return true;
}

// D3.1：堆叠层 = 水平切片（与 Time 槽同几何布局），层序即精渲调度单位
bool SeismicSliceRenderer::UpdateStackLayer(
    QOpenGLFunctions_3_3_Core *gl,
    int layerIdx,
    const SgyVolume &volume,
    int sampleIndex,
    const SgySliceImage &image) {
    if (!gl || !initialized_ || layerIdx < 0 || layerIdx >= kMaxStackLayers ||
        image.width <= 0 || image.height <= 0 ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4) {
        return false;
    }
    const auto vertices = BuildSliceVertices(volume, SgySliceType::Time, sampleIndex);
    const std::size_t layer = static_cast<std::size_t>(layerIdx);
    gl->glBindVertexArray(stackVaos_[layer]);
    gl->glBindBuffer(GL_ARRAY_BUFFER, stackVbos_[layer]);
    gl->glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(SliceVertex) * vertices.size()), vertices.data());
    gl->glBindTexture(GL_TEXTURE_2D, stackTextures_[layer]);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glBindVertexArray(0);
    stackReady_[layer] = true;
    return true;
}

void SeismicSliceRenderer::SetStackLayerVisible(int layerIdx, bool visible) {
    if (layerIdx >= 0 && layerIdx < kMaxStackLayers)
        stackVisibleLayer_[static_cast<std::size_t>(layerIdx)] = visible;
}

void SeismicSliceRenderer::SetSliceAlpha(float alpha) {
    sliceAlpha_ = std::clamp(alpha, 0.05f, 1.0f);
}

bool SeismicSliceRenderer::UpdateSlice(
    QOpenGLFunctions_3_3_Core *gl,
    SeismicSliceSlot slot,
    const SgyVolume &volume,
    SgySliceType type,
    int index,
    const SgySliceImage &image) {
    if (!gl || !initialized_ || image.width <= 0 || image.height <= 0 ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4) {
        return false;
    }

    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex >= vaos_.size()) {
        return false;
    }

    const auto vertices = BuildSliceVertices(volume, type, index);

    gl->glBindVertexArray(vaos_[slotIndex]);
    gl->glBindBuffer(GL_ARRAY_BUFFER, vbos_[slotIndex]);
    gl->glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(sizeof(SliceVertex) * vertices.size()), vertices.data());

    const unsigned int indices[] = {0, 1, 2, 0, 2, 3};
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebos_[slotIndex]);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    indexCounts_[slotIndex] = 6;

    gl->glBindTexture(GL_TEXTURE_2D, textures_[slotIndex]);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    GLint oldWidth = 0, oldHeight = 0;
    gl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &oldWidth);
    gl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &oldHeight);

    if (oldWidth == image.width && oldHeight == image.height) {
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width, image.height,
                            GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    } else {
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    }

    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glBindVertexArray(0);

    slotReady_[slotIndex] = true;
    return true;
}

bool SeismicSliceRenderer::UpdateLineSlice(
    QOpenGLFunctions_3_3_Core *gl,
    const SgyVolume &volume,
    const std::vector<glm::ivec2> &pathPoints,
    const SgySliceImage &image) {
    if (!gl || !initialized_ || image.width <= 1 || image.height <= 0 || image.rgba.empty() || pathPoints.size() < 2) {
        return false;
    }

    std::vector<float> segmentLengths;
    segmentLengths.reserve(pathPoints.size() - 1);
    float totalLength = 0.0f;
    for (size_t i = 1; i < pathPoints.size(); ++i) {
        const float di = static_cast<float>(pathPoints[i].x - pathPoints[i - 1].x);
        const float dx = static_cast<float>(pathPoints[i].y - pathPoints[i - 1].y);
        const float length = std::sqrt(di * di + dx * dx);
        segmentLengths.push_back(length);
        totalLength += length;
    }
    if (totalLength <= 1e-4f) {
        return false;
    }

    const float horizontalScale = HorizontalScale();
    const float heightScale = HeightScale();
    const size_t slotIndex = static_cast<size_t>(SeismicSliceSlot::Line);
    const int columns = std::clamp(image.width, 2, 2048);

    std::vector<SliceVertex> vertices;
    vertices.reserve(static_cast<std::size_t>(columns * 2));
    size_t segmentIndex = 0;
    float segmentStartDistance = 0.0f;

    for (int col = 0; col < columns; ++col) {
        const float targetDistance = columns <= 1
            ? 0.0f
            : (static_cast<float>(col) / static_cast<float>(columns - 1)) * totalLength;
        while (segmentIndex + 1 < segmentLengths.size() &&
               targetDistance > segmentStartDistance + segmentLengths[segmentIndex]) {
            segmentStartDistance += segmentLengths[segmentIndex];
            ++segmentIndex;
        }

        const float segmentLength = std::max(segmentLengths[segmentIndex], 1e-4f);
        const float t = std::clamp((targetDistance - segmentStartDistance) / segmentLength, 0.0f, 1.0f);
        const glm::ivec2 &a = pathPoints[segmentIndex];
        const glm::ivec2 &b = pathPoints[segmentIndex + 1];
        const float inlineNo = static_cast<float>(a.x) + static_cast<float>(b.x - a.x) * t;
        const float xlineNo = static_cast<float>(a.y) + static_cast<float>(b.y - a.y) * t;
        const float x = Normalize(static_cast<int>(xlineNo), volume.XlineMin(), volume.XlineMax(), horizontalScale);
        const float z = Normalize(static_cast<int>(inlineNo), volume.InlineMin(), volume.InlineMax(), horizontalScale);
        const float u = static_cast<float>(col) / static_cast<float>(columns - 1);
        vertices.push_back({{x, -heightScale * 0.5f, z}, {u, 0.0f}});
        vertices.push_back({{x, heightScale * 0.5f, z}, {u, 1.0f}});
    }

    std::vector<unsigned int> &indices = dynamicIndices_[slotIndex];
    indices.clear();
    indices.reserve(static_cast<std::size_t>((columns - 1) * 6));
    for (int col = 0; col < columns - 1; ++col) {
        const unsigned int bottom0 = static_cast<unsigned int>(col * 2);
        const unsigned int top0 = bottom0 + 1;
        const unsigned int bottom1 = bottom0 + 2;
        const unsigned int top1 = bottom0 + 3;
        indices.push_back(bottom0);
        indices.push_back(bottom1);
        indices.push_back(top1);
        indices.push_back(bottom0);
        indices.push_back(top1);
        indices.push_back(top0);
    }

    gl->glBindVertexArray(vaos_[slotIndex]);
    gl->glBindBuffer(GL_ARRAY_BUFFER, vbos_[slotIndex]);
    gl->glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(SliceVertex) * vertices.size()), vertices.data(), GL_DYNAMIC_DRAW);

    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebos_[slotIndex]);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(unsigned int) * indices.size()), indices.data(), GL_DYNAMIC_DRAW);

    gl->glBindTexture(GL_TEXTURE_2D, textures_[slotIndex]);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glBindVertexArray(0);

    indexCounts_[slotIndex] = static_cast<GLsizei>(indices.size());
    slotReady_[slotIndex] = true;
    return true;
}

void SeismicSliceRenderer::Render(
    QOpenGLFunctions_3_3_Core *gl,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const glm::mat4 &model) const {
    if (!gl || !initialized_ || !visible_ || !program_) {
        return;
    }

    program_->bind();
    const GLuint progId = program_->programId();

    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "model"), 1, GL_FALSE, glm::value_ptr(model));
    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "view"), 1, GL_FALSE, glm::value_ptr(view));
    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
    gl->glUniform1i(gl->glGetUniformLocation(progId, "sliceTexture"), 0);
    gl->glUniform1f(gl->glGetUniformLocation(progId, "uAlpha"), sliceAlpha_); // D3.3

    gl->glDisable(GL_CULL_FACE);
    gl->glEnable(GL_BLEND);
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl->glActiveTexture(GL_TEXTURE0);

    for (size_t slot = 0; slot < vaos_.size(); ++slot) {
        if (!slotReady_[slot] || !slotVisible_[slot]) {
            continue;
        }
        gl->glBindTexture(GL_TEXTURE_2D, textures_[slot]);
        gl->glBindVertexArray(vaos_[slot]);
        gl->glDrawElements(GL_TRIANGLES, indexCounts_[slot], GL_UNSIGNED_INT, nullptr);
    }

    // D3.1 体渲染堆叠层：深度测试开、深度写关 + 混合（水平面互不遮挡序无关）
    if (stackVisible_) {
        gl->glDepthMask(GL_FALSE);
        const float savedAlpha = sliceAlpha_;
        gl->glUniform1f(gl->glGetUniformLocation(progId, "uAlpha"),
                        std::clamp(savedAlpha * 0.55f, 0.05f, 1.0f));
        for (int layer = 0; layer < kMaxStackLayers; ++layer) {
            const std::size_t li = static_cast<std::size_t>(layer);
            if (!stackReady_[li] || !stackVisibleLayer_[li])
                continue;
            gl->glBindTexture(GL_TEXTURE_2D, stackTextures_[li]);
            gl->glBindVertexArray(stackVaos_[li]);
            gl->glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
        }
        gl->glUniform1f(gl->glGetUniformLocation(progId, "uAlpha"), savedAlpha);
        gl->glDepthMask(GL_TRUE);
    }

    gl->glBindVertexArray(0);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glDisable(GL_BLEND);
    gl->glEnable(GL_CULL_FACE);

    program_->release();
}

void SeismicSliceRenderer::SetSlotVisible(SeismicSliceSlot slot, bool value) {
    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex < slotVisible_.size()) {
        slotVisible_[slotIndex] = value;
    }
}

bool SeismicSliceRenderer::IsSlotVisible(SeismicSliceSlot slot) const {
    const size_t slotIndex = static_cast<size_t>(slot);
    return (slotIndex < slotVisible_.size()) ? slotVisible_[slotIndex] : false;
}

bool SeismicSliceRenderer::IsSlotReady(SeismicSliceSlot slot) const {
    const size_t slotIndex = static_cast<size_t>(slot);
    return (slotIndex < slotReady_.size()) ? slotReady_[slotIndex] : false;
}

void SeismicSliceRenderer::ClearSlot(QOpenGLFunctions_3_3_Core *gl, SeismicSliceSlot slot) {
    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex < slotReady_.size()) {
        slotReady_[slotIndex] = false;
        indexCounts_[slotIndex] = 0;
        dynamicIndices_[slotIndex].clear();
    }
}

} // namespace seismic
