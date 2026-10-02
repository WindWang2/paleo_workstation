// 层：数据
#pragma once

#include "../../domain/faultset.h"

#include <QString>
#include <QVector>

#include <vector>

// algorithms/faultsurface — 断棒条带成面与断距量算（纯数值）。
//
// 棒是剖面内 (traceFrac, 纵向样值)。地图位置由调用方给出的 SurveyFrame
// （仿射 inline/xline 标架）决定，不读投影服务。
// 单剖面断层拒绝成面（不向上下外推）。同一剖面两棒在 trace 与纵向都重叠
// 视为分叉，显式失败。沿走向转角超过 75° 视为走向突变，显式失败。
// 纵向域混用（TWT / depth）拒绝，不换算。

namespace paleo::faultsurf {

enum class SurfaceBuildStatus {
    Ok = 0,
    TooFewSections,
    EmptyStick,
    MixedDomain,
    Branching,
    DirectionBreak,
    BadFrame,
    BadSection,
    DegenerateMesh,
    NoIntersection
};

struct SurveyFrame {
    double originInline = 0;
    double originXline = 0;
    double originX = 0;
    double originY = 0;
    double inlineStepX = 0; // 每 +1 inline 的地图增量
    double inlineStepY = 1;
    double xlineStepX = 1;
    double xlineStepY = 0;
    double inlineMin = 0; // Xline 剖面上 traceFrac 0..1 对应的 inline 号
    double inlineMax = 1;
    double xlineMin = 0; // Inline 剖面上 traceFrac 0..1 对应的 xline 号
    double xlineMax = 1;
    double zScale = 1; // 地图 Z = 纵向样值 * zScale，Z 向下为正
};

struct SurfaceBuildResult {
    SurfaceBuildStatus status = SurfaceBuildStatus::Ok;
    QString message;
    paleo::fault::FaultSurfaceMesh mesh;
    bool ok() const { return status == SurfaceBuildStatus::Ok; }
};

struct Attitude {
    double dipDeg = 0;        // 0 水平，90 直立
    double dipAzimuthDeg = 0; // 自 +Y（北）顺时针到下倾方向
    double strikeDeg = 0;     // 倾向方位 − 90°（mod 360）
    double nx = 0;
    double ny = 0;
    double nz = 1; // 单位法向，nz >= 0（Z 向下）
};

struct MeshTopology {
    bool ok = false;
    int triangles = 0;
    int degenerate = 0;
    int boundaryEdges = 0;
    int nonManifoldEdges = 0;
    QString message;
};

struct HorizonPlane {
    double nx = 0;
    double ny = 0;
    double nz = 1;
    double d = 0; // nx*x + ny*y + nz*z = d
};

struct SlipSample {
    double x = 0;
    double y = 0;
    double z = 0;
    double along = 0;
    double heave = 0;
    double throwZ = 0;
};

struct SlipCurve {
    SurfaceBuildStatus status = SurfaceBuildStatus::Ok;
    QString message;
    QVector<SlipSample> samples;
    double length = 0; // 下盘交线弧长
    double throwMin = 0;
    double throwMax = 0;
    double heaveMin = 0;
    double heaveMax = 0;
    bool ok() const { return status == SurfaceBuildStatus::Ok && !samples.isEmpty(); }
};

struct SectionHit {
    double traceFrac = 0;
    double sample = 0;
    double x = 0;
    double y = 0;
    double z = 0;
};

struct SectionCut {
    SurfaceBuildStatus status = SurfaceBuildStatus::Ok;
    QString message;
    QVector<SectionHit> hits;
    bool ok() const { return status == SurfaceBuildStatus::Ok && !hits.isEmpty(); }
};

struct Triangle3 {
    double ax = 0, ay = 0, az = 0;
    double bx = 0, by = 0, bz = 0;
    double cx = 0, cy = 0, cz = 0;
};

SurfaceBuildResult buildFaultSurface(const paleo::fault::Fault &fault, const SurveyFrame &frame);

MeshTopology validateMeshTopology(const paleo::fault::FaultSurfaceMesh &mesh);

bool surfaceAttitude(const paleo::fault::FaultSurfaceMesh &mesh, Attitude *out, QString *error = nullptr);
bool attitudeAt(const paleo::fault::FaultSurfaceMesh &mesh, double x, double y, double z, Attitude *out,
                QString *error = nullptr);

SlipCurve measureSlip(const paleo::fault::FaultSurfaceMesh &mesh, const HorizonPlane &footwall,
                      const HorizonPlane &hanging);

SectionCut intersectSurfaceWithSection(const paleo::fault::FaultSurfaceMesh &mesh, const SurveyFrame &frame,
                                       const paleo::fault::FaultSectionRef &section);

bool segmentIntersectsMesh(double ax, double ay, double az, double bx, double by, double bz,
                           const paleo::fault::FaultSurfaceMesh &mesh);
bool segmentIntersectsTriangles(double ax, double ay, double az, double bx, double by, double bz,
                                const std::vector<Triangle3> &tris);

QString statusText(SurfaceBuildStatus status);

} // namespace paleo::faultsurf
