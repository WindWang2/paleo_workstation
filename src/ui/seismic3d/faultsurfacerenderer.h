// 层：视图
#pragma once

#include "../../domain/faultset.h"
#include "seismiccameracontroller.h"

#include <QColor>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace seismic {

// 地质坐标（X 东、Y 北、Z 向下）→ 视口（X 东、Y 向上、Z 北）。
struct FaultSceneMesh {
    std::vector<float> positions; // xyz，场景坐标
    std::vector<unsigned> triangleIndices;
    std::vector<unsigned> stickLineIndices;
    QColor color{229, 57, 53}; // 与剖面断棒同一数据符号色
    float alpha = 0.35f;
};

struct FaultSceneFit {
    glm::vec3 minBound{0.f};
    glm::vec3 maxBound{0.f};
    float zNear = 0.1f;
    float zFar = 1000.f;
    bool valid = false;
};

FaultSceneMesh makeFaultSceneMesh(const paleo::fault::FaultSurfaceMesh &mesh);

// 把相机装进断面包围盒，并给出能罩住包围盒的近远裁剪。
FaultSceneFit fitFaultSceneCamera(SeismicCameraController &camera, const FaultSceneMesh &mesh, float aspect);

// 八个角点都在 NDC 内（clip.w > 0）。
bool faultSceneBoundsInsideFrustum(const SeismicCameraController &camera, const FaultSceneFit &fit, float aspect,
                                   float ndcLimit = 1.02f);

class FaultSurfaceRenderer {
public:
    FaultSurfaceRenderer() = default;
    ~FaultSurfaceRenderer();

    FaultSurfaceRenderer(const FaultSurfaceRenderer &) = delete;
    FaultSurfaceRenderer &operator=(const FaultSurfaceRenderer &) = delete;

    bool Initialize(QOpenGLFunctions_3_3_Core *gl);
    void Cleanup(QOpenGLFunctions_3_3_Core *gl);
    bool Update(QOpenGLFunctions_3_3_Core *gl, const FaultSceneMesh &mesh);
    void Render(QOpenGLFunctions_3_3_Core *gl, const glm::mat4 &view, const glm::mat4 &projection) const;

    void setMesh(const FaultSceneMesh &mesh) { mesh_ = mesh; }
    [[nodiscard]] const FaultSceneMesh &mesh() const { return mesh_; }
    [[nodiscard]] int triangleCount() const { return int(mesh_.triangleIndices.size() / 3); }
    [[nodiscard]] bool IsInitialized() const { return initialized_; }

private:
    std::unique_ptr<QOpenGLShaderProgram> program_;
    FaultSceneMesh mesh_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    GLuint lineVao_ = 0;
    GLuint lineEbo_ = 0;
    GLsizei indexCount_ = 0;
    GLsizei lineIndexCount_ = 0;
    bool initialized_ = false;
};

} // namespace seismic
