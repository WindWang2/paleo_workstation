// 层：视图
#include "faultsurfacerenderer.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace seismic {
namespace {

glm::vec3 corner(const glm::vec3 &lo, const glm::vec3 &hi, int bit)
{
    return glm::vec3((bit & 1) ? hi.x : lo.x, (bit & 2) ? hi.y : lo.y, (bit & 4) ? hi.z : lo.z);
}

} // namespace

FaultSceneMesh makeFaultSceneMesh(const paleo::fault::FaultSurfaceMesh &mesh)
{
    FaultSceneMesh scene;
    scene.positions.reserve(static_cast<std::size_t>(mesh.vertices.size()) * 3);
    scene.triangleIndices.reserve(static_cast<std::size_t>(mesh.triangles.size()) * 3);
    for (const paleo::fault::FaultSurfaceVertex &v : mesh.vertices) {
        scene.positions.push_back(static_cast<float>(v.x));
        scene.positions.push_back(static_cast<float>(-v.z));
        scene.positions.push_back(static_cast<float>(v.y));
    }
    for (const paleo::fault::FaultSurfaceTriangle &t : mesh.triangles) {
        scene.triangleIndices.push_back(static_cast<unsigned>(t.a));
        scene.triangleIndices.push_back(static_cast<unsigned>(t.b));
        scene.triangleIndices.push_back(static_cast<unsigned>(t.c));
    }
    QHash<QString, QVector<int>> byStick;
    for (int i = 0; i < mesh.vertices.size(); ++i) {
        const QString id = mesh.vertices.at(i).stickId;
        if (!id.isEmpty() && mesh.vertices.at(i).pointIndex >= 0)
            byStick[id].append(i);
    }
    for (auto it = byStick.begin(); it != byStick.end(); ++it) {
        QVector<int> order = it.value();
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return mesh.vertices.at(a).pointIndex < mesh.vertices.at(b).pointIndex;
        });
        for (int i = 1; i < order.size(); ++i) {
            scene.stickLineIndices.push_back(static_cast<unsigned>(order[i - 1]));
            scene.stickLineIndices.push_back(static_cast<unsigned>(order[i]));
        }
    }
    return scene;
}

FaultSceneFit fitFaultSceneCamera(SeismicCameraController &camera, const FaultSceneMesh &mesh, float aspect)
{
    FaultSceneFit fit;
    if (mesh.positions.size() < 3)
        return fit;
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (std::size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
        const glm::vec3 p(mesh.positions[i], mesh.positions[i + 1], mesh.positions[i + 2]);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    fit.minBound = lo;
    fit.maxBound = hi;
    fit.valid = true;
    camera.FitToBounds(lo, hi, aspect);
    const float radius = std::max(0.1f, glm::length(hi - lo) * 0.5f);
    const float distance = std::max(camera.Distance(), 0.2f);
    fit.zNear = std::max(0.05f, (distance - radius) * 0.5f);
    fit.zFar = std::max(fit.zNear + 1.f, (distance + radius) * 1.5f);
    return fit;
}

bool faultSceneBoundsInsideFrustum(const SeismicCameraController &camera, const FaultSceneFit &fit, float aspect,
                                   float ndcLimit)
{
    if (!fit.valid)
        return false;
    const glm::mat4 view = camera.BuildViewMatrix();
    const glm::mat4 proj = camera.BuildProjectionMatrix(aspect, 45.f, fit.zNear, fit.zFar);
    for (int bit = 0; bit < 8; ++bit) {
        const glm::vec3 p = corner(fit.minBound, fit.maxBound, bit);
        const glm::vec4 clip = proj * view * glm::vec4(p, 1.f);
        if (!(clip.w > 0.f))
            return false;
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (std::fabs(ndc.x) > ndcLimit || std::fabs(ndc.y) > ndcLimit || std::fabs(ndc.z) > ndcLimit)
            return false;
    }
    return true;
}

FaultSurfaceRenderer::~FaultSurfaceRenderer() = default;

void FaultSurfaceRenderer::Cleanup(QOpenGLFunctions_3_3_Core *gl)
{
    if (!gl)
        return;
    if (ebo_)
        gl->glDeleteBuffers(1, &ebo_);
    if (lineEbo_)
        gl->glDeleteBuffers(1, &lineEbo_);
    if (vbo_)
        gl->glDeleteBuffers(1, &vbo_);
    if (vao_)
        gl->glDeleteVertexArrays(1, &vao_);
    if (lineVao_)
        gl->glDeleteVertexArrays(1, &lineVao_);
    ebo_ = vbo_ = vao_ = lineEbo_ = lineVao_ = 0;
    indexCount_ = lineIndexCount_ = 0;
    program_.reset();
    initialized_ = false;
}

bool FaultSurfaceRenderer::Initialize(QOpenGLFunctions_3_3_Core *gl)
{
    if (!gl)
        return false;
    Cleanup(gl);
    program_ = std::make_unique<QOpenGLShaderProgram>();
    static const char *kVert = R"GLSL(
#version 330 core
layout(location = 0) in vec3 position;
uniform mat4 view;
uniform mat4 projection;
void main() { gl_Position = projection * view * vec4(position, 1.0); }
)GLSL";
    static const char *kFrag = R"GLSL(
#version 330 core
uniform vec3 uColor;
uniform float uAlpha;
out vec4 fragColor;
void main() { fragColor = vec4(uColor, uAlpha); }
)GLSL";
    if (!program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVert) ||
        !program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag) || !program_->link())
        return false;
    gl->glGenVertexArrays(1, &vao_);
    gl->glGenVertexArrays(1, &lineVao_);
    gl->glGenBuffers(1, &vbo_);
    gl->glGenBuffers(1, &ebo_);
    gl->glGenBuffers(1, &lineEbo_);
    initialized_ = true;
    return true;
}

bool FaultSurfaceRenderer::Update(QOpenGLFunctions_3_3_Core *gl, const FaultSceneMesh &mesh)
{
    mesh_ = mesh;
    if (!gl || !initialized_)
        return false;
    indexCount_ = GLsizei(mesh.triangleIndices.size());
    lineIndexCount_ = GLsizei(mesh.stickLineIndices.size());
    gl->glBindVertexArray(vao_);
    gl->glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl->glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.positions.size() * sizeof(float)), mesh.positions.data(),
                     GL_STATIC_DRAW);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(mesh.triangleIndices.size() * sizeof(unsigned)),
                     mesh.triangleIndices.data(), GL_STATIC_DRAW);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);

    gl->glBindVertexArray(lineVao_);
    gl->glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lineEbo_);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(mesh.stickLineIndices.size() * sizeof(unsigned)),
                     mesh.stickLineIndices.data(), GL_STATIC_DRAW);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    gl->glBindVertexArray(0);
    return true;
}

void FaultSurfaceRenderer::Render(QOpenGLFunctions_3_3_Core *gl, const glm::mat4 &view,
                                  const glm::mat4 &projection) const
{
    if (!gl || !initialized_ || !program_ || indexCount_ < 3)
        return;
    program_->bind();
    const GLuint progId = program_->programId();
    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "view"), 1, GL_FALSE, glm::value_ptr(view));
    gl->glUniformMatrix4fv(gl->glGetUniformLocation(progId, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
    const QColor c = mesh_.color;
    gl->glUniform3f(gl->glGetUniformLocation(progId, "uColor"), c.redF(), c.greenF(), c.blueF());
    gl->glUniform1f(gl->glGetUniformLocation(progId, "uAlpha"), mesh_.alpha);
    gl->glEnable(GL_BLEND);
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDepthMask(GL_FALSE);
    gl->glBindVertexArray(vao_);
    gl->glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    gl->glUniform1f(gl->glGetUniformLocation(progId, "uAlpha"), 1.0f);
    if (lineIndexCount_ >= 2) {
        gl->glBindVertexArray(lineVao_);
        gl->glDrawElements(GL_LINES, lineIndexCount_, GL_UNSIGNED_INT, nullptr);
    }
    gl->glBindVertexArray(0);
    gl->glDepthMask(GL_TRUE);
    gl->glDisable(GL_BLEND);
    program_->release();
}

} // namespace seismic
