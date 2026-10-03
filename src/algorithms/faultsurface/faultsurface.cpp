// 层：数据
#include "faultsurface.h"

#include <QtMath>

#include <QMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>

namespace paleo::faultsurf {

namespace {

constexpr double kTurnLimitDeg = 75.0;
constexpr double kOverlapEps = 1e-4;

struct Vec3 {
    double x = 0;
    double y = 0;
    double z = 0;
};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double len(Vec3 a) { return std::sqrt(dot(a, a)); }
double clamp1(double v) { return std::max(-1.0, std::min(1.0, v)); }

bool finite3(Vec3 a)
{
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

struct PolyPoint {
    Vec3 p;
    QString stickId;
    int pointIndex = -1;
    double traceFrac = 0;
    double sample = 0;
};

struct SectionPoly {
    QString key;
    QVector<PolyPoint> pts;
    Vec3 mid;
};

void setMessage(QString *error, const QString &text)
{
    if (error)
        *error = text;
}

SurfaceBuildResult fail(SurfaceBuildStatus status, const QString &message)
{
    SurfaceBuildResult r;
    r.status = status;
    r.message = message;
    return r;
}

bool frameAxesOk(const SurveyFrame &frame, QString *why)
{
    if (!(frame.zScale > 0.0) || !std::isfinite(frame.zScale)) {
        setMessage(why, QStringLiteral("zScale 须为正有限值"));
        return false;
    }
    const double cross = frame.inlineStepX * frame.xlineStepY - frame.inlineStepY * frame.xlineStepX;
    const double scale = 1.0 + std::fabs(frame.inlineStepX) + std::fabs(frame.inlineStepY) +
                         std::fabs(frame.xlineStepX) + std::fabs(frame.xlineStepY);
    if (!(std::fabs(cross) > 1e-12 * scale * scale)) {
        setMessage(why, QStringLiteral("inline/xline 步长共线或为零，测网标架退化"));
        return false;
    }
    return true;
}

bool parsePath(const QString &pathId, QVector<QPair<double, double>> *ilxl, QString *why)
{
    ilxl->clear();
    const QStringList parts = pathId.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        setMessage(why, QStringLiteral("任意线路径至少需要两个 IL,XL 点"));
        return false;
    }
    for (const QString &part : parts) {
        const QStringList xy = part.split(QLatin1Char(','));
        if (xy.size() != 2) {
            setMessage(why, QStringLiteral("任意线路径点不是 il,xl：%1").arg(part));
            return false;
        }
        bool okIl = false;
        bool okXl = false;
        const double il = xy.at(0).trimmed().toDouble(&okIl);
        const double xl = xy.at(1).trimmed().toDouble(&okXl);
        if (!okIl || !okXl) {
            setMessage(why, QStringLiteral("任意线路径点无法解析：%1").arg(part));
            return false;
        }
        ilxl->append({il, xl});
    }
    return true;
}

Vec3 worldAt(const SurveyFrame &frame, double il, double xl, double sample)
{
    const double dil = il - frame.originInline;
    const double dxl = xl - frame.originXline;
    Vec3 p;
    p.x = frame.originX + dil * frame.inlineStepX + dxl * frame.xlineStepX;
    p.y = frame.originY + dil * frame.inlineStepY + dxl * frame.xlineStepY;
    p.z = sample * frame.zScale;
    return p;
}

bool locate(const SurveyFrame &frame, const paleo::fault::FaultSectionRef &sec, double traceFrac, double sample,
            Vec3 *out, QString *why)
{
    if (sec.kind == paleo::fault::FaultSectionRef::Inline) {
        if (!(std::fabs(frame.xlineMax - frame.xlineMin) > 1e-12)) {
            setMessage(why, QStringLiteral("Inline 剖面缺少 xline 范围"));
            return false;
        }
        const double xl = frame.xlineMin + traceFrac * (frame.xlineMax - frame.xlineMin);
        *out = worldAt(frame, sec.index, xl, sample);
        return finite3(*out);
    }
    if (sec.kind == paleo::fault::FaultSectionRef::Xline) {
        if (!(std::fabs(frame.inlineMax - frame.inlineMin) > 1e-12)) {
            setMessage(why, QStringLiteral("Xline 剖面缺少 inline 范围"));
            return false;
        }
        const double il = frame.inlineMin + traceFrac * (frame.inlineMax - frame.inlineMin);
        *out = worldAt(frame, il, sec.index, sample);
        return finite3(*out);
    }
    QVector<QPair<double, double>> path;
    if (!parsePath(sec.pathId, &path, why))
        return false;
    QVector<double> acc(path.size(), 0.0);
    double total = 0;
    for (int i = 1; i < path.size(); ++i) {
        const Vec3 a = worldAt(frame, path[i - 1].first, path[i - 1].second, 0);
        const Vec3 b = worldAt(frame, path[i].first, path[i].second, 0);
        total += std::hypot(b.x - a.x, b.y - a.y);
        acc[i] = total;
    }
    if (!(total > 1e-12)) {
        setMessage(why, QStringLiteral("任意线路径长度为零"));
        return false;
    }
    const double target = std::max(0.0, std::min(1.0, traceFrac)) * total;
    int seg = 1;
    while (seg < acc.size() - 1 && acc[seg] < target)
        ++seg;
    const double segLen = acc[seg] - acc[seg - 1];
    const double u = segLen > 1e-12 ? (target - acc[seg - 1]) / segLen : 0;
    const double il = path[seg - 1].first + u * (path[seg].first - path[seg - 1].first);
    const double xl = path[seg - 1].second + u * (path[seg].second - path[seg - 1].second);
    *out = worldAt(frame, il, xl, sample);
    return finite3(*out);
}

bool traceAndSample(const SurveyFrame &frame, const paleo::fault::FaultSectionRef &sec, Vec3 p, double *traceFrac,
                    double *sample, QString *why)
{
    *sample = p.z / frame.zScale;
    if (sec.kind == paleo::fault::FaultSectionRef::Inline) {
        const double step2 = frame.xlineStepX * frame.xlineStepX + frame.xlineStepY * frame.xlineStepY;
        const Vec3 origin = worldAt(frame, sec.index, frame.originXline, 0);
        const double along = ((p.x - origin.x) * frame.xlineStepX + (p.y - origin.y) * frame.xlineStepY) / step2;
        const double xl = frame.originXline + along;
        *traceFrac = (xl - frame.xlineMin) / (frame.xlineMax - frame.xlineMin);
        return std::isfinite(*traceFrac);
    }
    if (sec.kind == paleo::fault::FaultSectionRef::Xline) {
        const double step2 = frame.inlineStepX * frame.inlineStepX + frame.inlineStepY * frame.inlineStepY;
        const Vec3 origin = worldAt(frame, frame.originInline, sec.index, 0);
        const double along =
            ((p.x - origin.x) * frame.inlineStepX + (p.y - origin.y) * frame.inlineStepY) / step2;
        const double il = frame.originInline + along;
        *traceFrac = (il - frame.inlineMin) / (frame.inlineMax - frame.inlineMin);
        return std::isfinite(*traceFrac);
    }
    QVector<QPair<double, double>> path;
    if (!parsePath(sec.pathId, &path, why))
        return false;
    double best = std::numeric_limits<double>::max();
    double bestAcc = 0;
    double total = 0;
    for (int i = 1; i < path.size(); ++i) {
        const Vec3 a = worldAt(frame, path[i - 1].first, path[i - 1].second, 0);
        const Vec3 b = worldAt(frame, path[i].first, path[i].second, 0);
        const double abx = b.x - a.x;
        const double aby = b.y - a.y;
        const double ab2 = abx * abx + aby * aby;
        double u = 0;
        if (ab2 > 1e-18)
            u = std::max(0.0, std::min(1.0, ((p.x - a.x) * abx + (p.y - a.y) * aby) / ab2));
        const double dx = p.x - (a.x + u * abx);
        const double dy = p.y - (a.y + u * aby);
        const double d2 = dx * dx + dy * dy;
        if (d2 < best) {
            best = d2;
            bestAcc = total + u * std::sqrt(ab2);
        }
        total += std::sqrt(ab2);
    }
    if (!(total > 1e-12)) {
        setMessage(why, QStringLiteral("任意线路径长度为零"));
        return false;
    }
    *traceFrac = bestAcc / total;
    return true;
}

double triArea(Vec3 a, Vec3 b, Vec3 c) { return 0.5 * len(cross(b - a, c - a)); }

struct Seg {
    Vec3 a;
    Vec3 b;
};

bool nearlySame(Vec3 a, Vec3 b, double tol) { return len(a - b) <= tol; }

QVector<Vec3> chainSegments(const QVector<Seg> &segs, double tol)
{
    if (segs.isEmpty())
        return {};
    QVector<char> used(segs.size(), 0);
    QVector<Vec3> poly;
    used[0] = 1;
    poly.append(segs[0].a);
    poly.append(segs[0].b);
    bool grew = true;
    while (grew) {
        grew = false;
        for (int i = 0; i < segs.size(); ++i) {
            if (used[i])
                continue;
            if (nearlySame(segs[i].a, poly.last(), tol)) {
                poly.append(segs[i].b);
                used[i] = 1;
                grew = true;
            } else if (nearlySame(segs[i].b, poly.last(), tol)) {
                poly.append(segs[i].a);
                used[i] = 1;
                grew = true;
            } else if (nearlySame(segs[i].a, poly.first(), tol)) {
                poly.prepend(segs[i].b);
                used[i] = 1;
                grew = true;
            } else if (nearlySame(segs[i].b, poly.first(), tol)) {
                poly.prepend(segs[i].a);
                used[i] = 1;
                grew = true;
            }
        }
    }
    QVector<Vec3> collapsed;
    for (const Vec3 &p : poly) {
        if (collapsed.isEmpty() || !nearlySame(collapsed.last(), p, tol))
            collapsed.append(p);
    }
    return collapsed;
}

double planeSide(const HorizonPlane &plane, Vec3 p)
{
    return plane.nx * p.x + plane.ny * p.y + plane.nz * p.z - plane.d;
}

Vec3 lerp(Vec3 a, Vec3 b, double t) { return a + (b - a) * t; }

int sideSign(double s, double eps)
{
    if (s > eps)
        return 1;
    if (s < -eps)
        return -1;
    return 0;
}

bool cutTriangle(Vec3 a, Vec3 b, Vec3 c, const HorizonPlane &plane, Seg *seg)
{
    const double eps = 1e-9 * (1.0 + std::fabs(plane.d) + len(a) + len(b) + len(c));
    const Vec3 v[3] = {a, b, c};
    const double s[3] = {planeSide(plane, a), planeSide(plane, b), planeSide(plane, c)};
    const int g[3] = {sideSign(s[0], eps), sideSign(s[1], eps), sideSign(s[2], eps)};
    QVector<Vec3> hits;
    auto add = [&](Vec3 p) {
        for (const Vec3 &h : hits)
            if (nearlySame(h, p, eps * 10))
                return;
        hits.append(p);
    };
    for (int i = 0; i < 3; ++i) {
        if (g[i] == 0)
            add(v[i]);
        const int j = (i + 1) % 3;
        if (g[i] * g[j] < 0) {
            const double t = s[i] / (s[i] - s[j]);
            add(lerp(v[i], v[j], t));
        }
    }
    if (hits.size() < 2)
        return false;
    *seg = {hits[0], hits[1]};
    if (nearlySame(seg->a, seg->b, eps * 10))
        return false;
    return true;
}

QVector<Seg> meshPlaneSegments(const paleo::fault::FaultSurfaceMesh &mesh, const HorizonPlane &plane)
{
    QVector<Seg> segs;
    const double nlen = std::sqrt(plane.nx * plane.nx + plane.ny * plane.ny + plane.nz * plane.nz);
    if (!(nlen > 1e-15))
        return segs;
    for (const paleo::fault::FaultSurfaceTriangle &t : mesh.triangles) {
        if (t.a < 0 || t.b < 0 || t.c < 0 || t.a >= mesh.vertices.size() || t.b >= mesh.vertices.size() ||
            t.c >= mesh.vertices.size())
            continue;
        const auto &va = mesh.vertices[t.a];
        const auto &vb = mesh.vertices[t.b];
        const auto &vc = mesh.vertices[t.c];
        Seg seg;
        if (cutTriangle({va.x, va.y, va.z}, {vb.x, vb.y, vb.z}, {vc.x, vc.y, vc.z}, plane, &seg))
            segs.append(seg);
    }
    return segs;
}

bool segmentHitsTriangle(Vec3 a, Vec3 b, Vec3 v0, Vec3 v1, Vec3 v2)
{
    const Vec3 dir = b - a;
    const Vec3 e1 = v1 - v0;
    const Vec3 e2 = v2 - v0;
    const Vec3 pvec = cross(dir, e2);
    const double det = dot(e1, pvec);
    const double scale = 1.0 + len(e1) + len(e2) + len(dir);
    if (std::fabs(det) <= 1e-14 * scale * scale * scale)
        return false;
    const double inv = 1.0 / det;
    const Vec3 tvec = a - v0;
    const double u = dot(tvec, pvec) * inv;
    if (u < -1e-8 || u > 1.0 + 1e-8)
        return false;
    const Vec3 qvec = cross(tvec, e1);
    const double v = dot(dir, qvec) * inv;
    if (v < -1e-8 || u + v > 1.0 + 1e-8)
        return false;
    const double t = dot(e2, qvec) * inv;
    return t > 1e-8 && t < 1.0 - 1e-8;
}

Attitude attitudeFromNormal(Vec3 n)
{
    const double nlen = len(n);
    Attitude att;
    if (!(nlen > 1e-15))
        return att;
    n = n * (1.0 / nlen);
    if (n.z < 0)
        n = n * -1.0;
    if (std::fabs(n.z) <= 1e-12 && (n.x < 0 || (std::fabs(n.x) <= 1e-15 && n.y < 0)))
        n = n * -1.0;
    att.nx = n.x;
    att.ny = n.y;
    att.nz = n.z;
    att.dipDeg = qRadiansToDegrees(std::acos(clamp1(std::fabs(n.z))));
    double east = 0;
    double north = 0;
    if (std::fabs(n.z) <= 1e-12) {
        east = n.x;
        north = n.y;
    } else {
        const double sign = n.z >= 0 ? 1.0 : -1.0;
        east = -sign * n.x;
        north = -sign * n.y;
    }
    const double h = std::hypot(east, north);
    if (h > 1e-15) {
        att.dipAzimuthDeg = qRadiansToDegrees(std::atan2(east, north));
        if (att.dipAzimuthDeg < 0)
            att.dipAzimuthDeg += 360.0;
    }
    att.strikeDeg = std::fmod(att.dipAzimuthDeg - 90.0 + 360.0, 360.0);
    return att;
}

} // namespace

QString statusText(SurfaceBuildStatus status)
{
    switch (status) {
    case SurfaceBuildStatus::Ok:
        return QStringLiteral("ok");
    case SurfaceBuildStatus::TooFewSections:
        return QStringLiteral("单剖面断层不成面");
    case SurfaceBuildStatus::EmptyStick:
        return QStringLiteral("断棒点数不足");
    case SurfaceBuildStatus::MixedDomain:
        return QStringLiteral("棒的时深域不一致");
    case SurfaceBuildStatus::Branching:
        return QStringLiteral("分叉棒");
    case SurfaceBuildStatus::DirectionBreak:
        return QStringLiteral("走向突变");
    case SurfaceBuildStatus::BadFrame:
        return QStringLiteral("测网标架无效");
    case SurfaceBuildStatus::BadSection:
        return QStringLiteral("剖面无法定位");
    case SurfaceBuildStatus::DegenerateMesh:
        return QStringLiteral("三角网退化");
    case SurfaceBuildStatus::NoIntersection:
        return QStringLiteral("无交线");
    }
    return QStringLiteral("未知");
}

MeshTopology validateMeshTopology(const paleo::fault::FaultSurfaceMesh &mesh)
{
    MeshTopology topo;
    topo.triangles = mesh.triangles.size();
    if (mesh.vertices.size() < 3 || mesh.triangles.isEmpty()) {
        topo.message = QStringLiteral("断面没有三角形");
        return topo;
    }
    double scale = 1;
    for (const auto &v : mesh.vertices)
        scale = std::max(scale, 1.0 + std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z));
    std::map<std::uint64_t, int> edges;
    for (const auto &t : mesh.triangles) {
        if (t.a < 0 || t.b < 0 || t.c < 0 || t.a >= mesh.vertices.size() || t.b >= mesh.vertices.size() ||
            t.c >= mesh.vertices.size() || t.a == t.b || t.b == t.c || t.a == t.c) {
            ++topo.degenerate;
            continue;
        }
        const Vec3 a{mesh.vertices[t.a].x, mesh.vertices[t.a].y, mesh.vertices[t.a].z};
        const Vec3 b{mesh.vertices[t.b].x, mesh.vertices[t.b].y, mesh.vertices[t.b].z};
        const Vec3 c{mesh.vertices[t.c].x, mesh.vertices[t.c].y, mesh.vertices[t.c].z};
        if (!(triArea(a, b, c) > 1e-12 * scale)) {
            ++topo.degenerate;
            continue;
        }
        const int idx[3] = {t.a, t.b, t.c};
        for (int e = 0; e < 3; ++e) {
            unsigned lo = static_cast<unsigned>(std::min(idx[e], idx[(e + 1) % 3]));
            unsigned hi = static_cast<unsigned>(std::max(idx[e], idx[(e + 1) % 3]));
            const std::uint64_t key = (std::uint64_t(lo) << 32) | hi;
            edges[key] += 1;
        }
    }
    for (const auto &entry : edges) {
        if (entry.second == 1)
            ++topo.boundaryEdges;
        else if (entry.second > 2)
            ++topo.nonManifoldEdges;
    }
    topo.ok = topo.degenerate == 0 && topo.nonManifoldEdges == 0 && topo.boundaryEdges > 0 &&
              topo.triangles > 0;
    if (!topo.ok)
        topo.message = QStringLiteral("退化 %1，非流形边 %2，边界边 %3")
                           .arg(topo.degenerate)
                           .arg(topo.nonManifoldEdges)
                           .arg(topo.boundaryEdges);
    return topo;
}

SurfaceBuildResult buildFaultSurface(const paleo::fault::Fault &fault, const SurveyFrame &frame)
{
    QString frameWhy;
    if (!frameAxesOk(frame, &frameWhy))
        return fail(SurfaceBuildStatus::BadFrame, frameWhy);
    if (fault.sticks.size() < 2)
        return fail(SurfaceBuildStatus::TooFewSections, QStringLiteral("单剖面断层不成面（拒绝向上下外推）"));

    const auto domain0 = fault.sticks.first().verticalDomain;
    for (const auto &stick : fault.sticks) {
        if (stick.verticalDomain != domain0)
            return fail(SurfaceBuildStatus::MixedDomain,
                        QStringLiteral("断层 %1 的棒混用 TWT 与深度，成面前拒绝")
                            .arg(fault.name.isEmpty() ? fault.id : fault.name));
        if (stick.points.size() < 2)
            return fail(SurfaceBuildStatus::EmptyStick,
                        QStringLiteral("棒 %1 少于 2 个点").arg(stick.id));
    }

    struct StickBox {
        const paleo::fault::FaultStick *stick = nullptr;
        double t0 = 0, t1 = 0, z0 = 0, z1 = 0;
    };
    QMap<QString, QVector<StickBox>> grouped;
    for (const auto &stick : fault.sticks) {
        StickBox box;
        box.stick = &stick;
        box.t0 = box.t1 = stick.points.first().first;
        box.z0 = box.z1 = stick.points.first().second;
        for (const auto &pt : stick.points) {
            box.t0 = std::min(box.t0, pt.first);
            box.t1 = std::max(box.t1, pt.first);
            box.z0 = std::min(box.z0, pt.second);
            box.z1 = std::max(box.z1, pt.second);
        }
        grouped[stick.section.matchKey()].append(box);
    }
    if (grouped.size() < 2)
        return fail(SurfaceBuildStatus::TooFewSections, QStringLiteral("单剖面断层不成面（拒绝向上下外推）"));

    QVector<SectionPoly> sections;
    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        auto boxes = it.value();
        const auto overlap = [](double a0, double a1, double b0, double b1) {
            return a1 > b0 + kOverlapEps && b1 > a0 + kOverlapEps;
        };
        for (int i = 0; i < boxes.size(); ++i) {
            for (int j = i + 1; j < boxes.size(); ++j) {
                if (overlap(boxes[i].t0, boxes[i].t1, boxes[j].t0, boxes[j].t1) &&
                    overlap(boxes[i].z0, boxes[i].z1, boxes[j].z0, boxes[j].z1)) {
                    return fail(SurfaceBuildStatus::Branching,
                                QStringLiteral("剖面 %1 上的棒 %2 与 %3 重叠，分叉不成面")
                                    .arg(it.key(), boxes[i].stick->id, boxes[j].stick->id));
                }
            }
        }
        std::sort(boxes.begin(), boxes.end(), [](const StickBox &a, const StickBox &b) {
            if (std::fabs(a.z0 - b.z0) > kOverlapEps)
                return a.z0 < b.z0;
            return a.t0 < b.t0;
        });
        SectionPoly sec;
        sec.key = it.key();
        for (const StickBox &box : boxes) {
            for (int pi = 0; pi < box.stick->points.size(); ++pi) {
                const auto &pt = box.stick->points.at(pi);
                Vec3 p;
                QString why;
                if (!locate(frame, box.stick->section, pt.first, pt.second, &p, &why))
                    return fail(SurfaceBuildStatus::BadSection, why);
                PolyPoint pp;
                pp.p = p;
                pp.stickId = box.stick->id;
                pp.pointIndex = pi;
                pp.traceFrac = pt.first;
                pp.sample = pt.second;
                if (!sec.pts.isEmpty() && len(sec.pts.last().p - pp.p) <= 1e-9 * (1.0 + len(pp.p)))
                    continue;
                sec.pts.append(pp);
            }
        }
        if (sec.pts.size() < 2)
            return fail(SurfaceBuildStatus::EmptyStick, QStringLiteral("剖面 %1 折叠后少于 2 个点").arg(sec.key));
        Vec3 mid;
        for (const PolyPoint &pp : sec.pts)
            mid = mid + pp.p;
        sec.mid = mid * (1.0 / sec.pts.size());
        sections.append(sec);
    }

    Vec3 centroid;
    for (const SectionPoly &sec : sections)
        centroid = centroid + sec.mid;
    centroid = centroid * (1.0 / sections.size());
    double xx = 0, xy = 0, yy = 0;
    for (const SectionPoly &sec : sections) {
        const double dx = sec.mid.x - centroid.x;
        const double dy = sec.mid.y - centroid.y;
        xx += dx * dx;
        xy += dx * dy;
        yy += dy * dy;
    }
    if (!(xx + yy > 1e-10))
        return fail(SurfaceBuildStatus::DirectionBreak, QStringLiteral("剖面位置重合，无法沿走向排序"));
    const double ang = 0.5 * std::atan2(2.0 * xy, xx - yy);
    const double ax = std::cos(ang);
    const double ay = std::sin(ang);
    std::sort(sections.begin(), sections.end(), [&](const SectionPoly &a, const SectionPoly &b) {
        const double pa = (a.mid.x - centroid.x) * ax + (a.mid.y - centroid.y) * ay;
        const double pb = (b.mid.x - centroid.x) * ax + (b.mid.y - centroid.y) * ay;
        return pa < pb;
    });
    for (int i = 1; i + 1 < sections.size(); ++i) {
        const Vec3 v1 = sections[i].mid - sections[i - 1].mid;
        const Vec3 v2 = sections[i + 1].mid - sections[i].mid;
        const double l1 = std::hypot(v1.x, v1.y);
        const double l2 = std::hypot(v2.x, v2.y);
        if (l1 < 1e-8 || l2 < 1e-8)
            return fail(SurfaceBuildStatus::DirectionBreak, QStringLiteral("剖面沿走向重叠"));
        const double turn = qRadiansToDegrees(std::acos(clamp1((v1.x * v2.x + v1.y * v2.y) / (l1 * l2))));
        if (turn > kTurnLimitDeg)
            return fail(SurfaceBuildStatus::DirectionBreak,
                        QStringLiteral("剖面走向转角 %1° 超过 %2°").arg(turn, 0, 'f', 1).arg(kTurnLimitDeg, 0, 'f', 0));
    }

    const Vec3 ref = sections.first().pts.last().p - sections.first().pts.first().p;
    for (int s = 1; s < sections.size(); ++s) {
        const Vec3 dir = sections[s].pts.last().p - sections[s].pts.first().p;
        if (dot(ref, dir) < 0)
            std::reverse(sections[s].pts.begin(), sections[s].pts.end());
    }
    Vec3 refH{ref.x, ref.y, 0};
    if (len(refH) > 1e-6) {
        for (int s = 1; s < sections.size(); ++s) {
            const Vec3 dir = sections[s].pts.last().p - sections[s].pts.first().p;
            const Vec3 dirH{dir.x, dir.y, 0};
            if (len(dirH) <= 1e-6)
                continue;
            const double turn =
                qRadiansToDegrees(std::acos(clamp1(dot(refH, dirH) / (len(refH) * len(dirH)))));
            if (turn > kTurnLimitDeg)
                return fail(SurfaceBuildStatus::DirectionBreak,
                            QStringLiteral("棒走向夹角 %1° 超过 %2°").arg(turn, 0, 'f', 1).arg(kTurnLimitDeg, 0, 'f', 0));
        }
    }

    paleo::fault::FaultSurfaceMesh mesh;
    QVector<int> start(sections.size());
    QVector<int> count(sections.size());
    for (int s = 0; s < sections.size(); ++s) {
        start[s] = mesh.vertices.size();
        count[s] = sections[s].pts.size();
        for (const PolyPoint &pp : sections[s].pts) {
            paleo::fault::FaultSurfaceVertex v;
            v.x = pp.p.x;
            v.y = pp.p.y;
            v.z = pp.p.z;
            v.stickId = pp.stickId;
            v.pointIndex = pp.pointIndex;
            mesh.vertices.append(v);
            if (!mesh.stickOrder.contains(pp.stickId))
                mesh.stickOrder.append(pp.stickId);
        }
    }

    auto paramOf = [](const QVector<PolyPoint> &pts) {
        QVector<double> t(pts.size(), 0.0);
        double total = 0;
        for (int i = 1; i < pts.size(); ++i) {
            total += len(pts[i].p - pts[i - 1].p);
            t[i] = total;
        }
        if (total > 1e-15) {
            for (double &v : t)
                v /= total;
        }
        return t;
    };

    for (int s = 0; s + 1 < sections.size(); ++s) {
        const QVector<double> ta = paramOf(sections[s].pts);
        const QVector<double> tb = paramOf(sections[s + 1].pts);
        int i = 0;
        int j = 0;
        const int n = count[s];
        const int m = count[s + 1];
        const int baseA = start[s];
        const int baseB = start[s + 1];
        while (i < n - 1 || j < m - 1) {
            paleo::fault::FaultSurfaceTriangle tri;
            tri.a = baseA + i;
            tri.b = baseB + j;
            if (i == n - 1) {
                tri.c = baseB + j + 1;
                ++j;
            } else if (j == m - 1) {
                tri.c = baseA + i + 1;
                ++i;
            } else if (ta[i + 1] <= tb[j + 1]) {
                tri.c = baseA + i + 1;
                ++i;
            } else {
                tri.c = baseB + j + 1;
                ++j;
            }
            mesh.triangles.append(tri);
        }
    }

    Vec3 accum;
    for (const auto &t : mesh.triangles) {
        const Vec3 a{mesh.vertices[t.a].x, mesh.vertices[t.a].y, mesh.vertices[t.a].z};
        const Vec3 b{mesh.vertices[t.b].x, mesh.vertices[t.b].y, mesh.vertices[t.b].z};
        const Vec3 c{mesh.vertices[t.c].x, mesh.vertices[t.c].y, mesh.vertices[t.c].z};
        accum = accum + cross(b - a, c - a);
    }
    if (accum.z < 0)
        accum = accum * -1.0;
    for (auto &t : mesh.triangles) {
        const Vec3 a{mesh.vertices[t.a].x, mesh.vertices[t.a].y, mesh.vertices[t.a].z};
        const Vec3 b{mesh.vertices[t.b].x, mesh.vertices[t.b].y, mesh.vertices[t.b].z};
        const Vec3 c{mesh.vertices[t.c].x, mesh.vertices[t.c].y, mesh.vertices[t.c].z};
        if (dot(cross(b - a, c - a), accum) < 0)
            std::swap(t.b, t.c);
    }

    const MeshTopology topo = validateMeshTopology(mesh);
    if (!topo.ok)
        return fail(SurfaceBuildStatus::DegenerateMesh, topo.message);
    SurfaceBuildResult result;
    result.mesh = mesh;
    result.message = QStringLiteral("成面 %1 顶点 %2 三角形")
                         .arg(mesh.vertices.size())
                         .arg(mesh.triangles.size());
    return result;
}

bool surfaceAttitude(const paleo::fault::FaultSurfaceMesh &mesh, Attitude *out, QString *error)
{
    if (!out) {
        setMessage(error, QStringLiteral("产状输出为空"));
        return false;
    }
    Vec3 accum;
    for (const auto &t : mesh.triangles) {
        if (t.a < 0 || t.b < 0 || t.c < 0 || t.a >= mesh.vertices.size() || t.b >= mesh.vertices.size() ||
            t.c >= mesh.vertices.size())
            continue;
        const Vec3 a{mesh.vertices[t.a].x, mesh.vertices[t.a].y, mesh.vertices[t.a].z};
        const Vec3 b{mesh.vertices[t.b].x, mesh.vertices[t.b].y, mesh.vertices[t.b].z};
        const Vec3 c{mesh.vertices[t.c].x, mesh.vertices[t.c].y, mesh.vertices[t.c].z};
        accum = accum + cross(b - a, c - a);
    }
    if (!(len(accum) > 1e-12)) {
        setMessage(error, QStringLiteral("断面法向退化"));
        return false;
    }
    *out = attitudeFromNormal(accum);
    return true;
}

bool attitudeAt(const paleo::fault::FaultSurfaceMesh &mesh, double x, double y, double z, Attitude *out,
                QString *error)
{
    if (!out) {
        setMessage(error, QStringLiteral("产状输出为空"));
        return false;
    }
    const Vec3 p{x, y, z};
    double best = std::numeric_limits<double>::max();
    bool found = false;
    Vec3 bestN;
    for (const auto &t : mesh.triangles) {
        if (t.a < 0 || t.b < 0 || t.c < 0 || t.a >= mesh.vertices.size() || t.b >= mesh.vertices.size() ||
            t.c >= mesh.vertices.size())
            continue;
        const Vec3 a{mesh.vertices[t.a].x, mesh.vertices[t.a].y, mesh.vertices[t.a].z};
        const Vec3 b{mesh.vertices[t.b].x, mesh.vertices[t.b].y, mesh.vertices[t.b].z};
        const Vec3 c{mesh.vertices[t.c].x, mesh.vertices[t.c].y, mesh.vertices[t.c].z};
        const Vec3 n = cross(b - a, c - a);
        const double nlen = len(n);
        if (!(nlen > 1e-15))
            continue;
        const Vec3 unit = n * (1.0 / nlen);
        const double dist = std::fabs(dot(unit, p - a));
        const Vec3 v0 = b - a;
        const Vec3 v1 = c - a;
        const Vec3 v2 = p - a;
        const double d00 = dot(v0, v0);
        const double d01 = dot(v0, v1);
        const double d11 = dot(v1, v1);
        const double d20 = dot(v2, v0);
        const double d21 = dot(v2, v1);
        const double denom = d00 * d11 - d01 * d01;
        if (std::fabs(denom) < 1e-18)
            continue;
        const double v = (d11 * d20 - d01 * d21) / denom;
        const double w = (d00 * d21 - d01 * d20) / denom;
        const double u = 1.0 - v - w;
        const bool inside = u >= -1e-4 && v >= -1e-4 && w >= -1e-4;
        const double score = inside ? dist : dist + 1e6;
        if (score < best) {
            best = score;
            bestN = n;
            found = true;
        }
    }
    if (!found) {
        setMessage(error, QStringLiteral("点不落在断面上"));
        return false;
    }
    *out = attitudeFromNormal(bestN);
    return true;
}

SlipCurve measureSlip(const paleo::fault::FaultSurfaceMesh &mesh, const HorizonPlane &footwall,
                      const HorizonPlane &hanging)
{
    SlipCurve curve;
    if (mesh.triangles.isEmpty()) {
        curve.status = SurfaceBuildStatus::DegenerateMesh;
        curve.message = QStringLiteral("断面为空");
        return curve;
    }
    double scale = 1;
    for (const auto &v : mesh.vertices)
        scale = std::max(scale, 1.0 + std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z));
    const double tol = 1e-6 * scale;
    const QVector<Vec3> foot = chainSegments(meshPlaneSegments(mesh, footwall), tol);
    const QVector<Vec3> hang = chainSegments(meshPlaneSegments(mesh, hanging), tol);
    if (foot.size() < 2 || hang.size() < 2) {
        curve.status = SurfaceBuildStatus::NoIntersection;
        curve.message = QStringLiteral("层位面与断面没有成对交线");
        return curve;
    }
    for (int i = 1; i < foot.size(); ++i)
        curve.length += len(foot[i] - foot[i - 1]);

    const Vec3 strikeRaw = foot.last() - foot.first();
    Vec3 strike{strikeRaw.x, strikeRaw.y, 0};
    if (!(len(strike) > 1e-9)) {
        curve.status = SurfaceBuildStatus::DegenerateMesh;
        curve.message = QStringLiteral("交线没有走向长度");
        return curve;
    }
    strike = strike * (1.0 / len(strike));
    const auto coord = [&](Vec3 p) { return (p.x - foot.first().x) * strike.x + (p.y - foot.first().y) * strike.y; };
    auto sorted = [](QVector<Vec3> poly, const auto &coordFn) {
        std::sort(poly.begin(), poly.end(), [&](Vec3 a, Vec3 b) { return coordFn(a) < coordFn(b); });
        return poly;
    };
    const QVector<Vec3> footS = sorted(foot, coord);
    const QVector<Vec3> hangS = sorted(hang, coord);
    const double s0 = std::max(coord(footS.first()), coord(hangS.first()));
    const double s1 = std::min(coord(footS.last()), coord(hangS.last()));
    if (!(s1 > s0 + tol)) {
        curve.status = SurfaceBuildStatus::NoIntersection;
        curve.message = QStringLiteral("上下盘交线沿走向没有重叠");
        return curve;
    }
    auto at = [&](const QVector<Vec3> &poly, double s) {
        for (int i = 1; i < poly.size(); ++i) {
            const double c0 = coord(poly[i - 1]);
            const double c1 = coord(poly[i]);
            if ((s >= c0 && s <= c1) || (s >= c1 && s <= c0)) {
                const double denom = c1 - c0;
                const double u = std::fabs(denom) > 1e-15 ? (s - c0) / denom : 0;
                return lerp(poly[i - 1], poly[i], u);
            }
        }
        return poly.last();
    };
    constexpr int kSamples = 9;
    double throwMin = 0;
    double throwMax = 0;
    double heaveMin = 0;
    double heaveMax = 0;
    Vec3 prevMid;
    for (int i = 0; i < kSamples; ++i) {
        const double u = kSamples == 1 ? 0.5 : double(i) / double(kSamples - 1);
        const double s = s0 + u * (s1 - s0);
        const Vec3 pf = at(footS, s);
        const Vec3 ph = at(hangS, s);
        const Vec3 mid = (pf + ph) * 0.5;
        const double dx = ph.x - pf.x;
        const double dy = ph.y - pf.y;
        const double alongSep = dx * strike.x + dy * strike.y;
        const double px = dx - alongSep * strike.x;
        const double py = dy - alongSep * strike.y;
        SlipSample sample;
        sample.x = mid.x;
        sample.y = mid.y;
        sample.z = mid.z;
        sample.throwZ = std::fabs(ph.z - pf.z);
        sample.heave = std::hypot(px, py);
        if (i == 0) {
            sample.along = 0;
            throwMin = throwMax = sample.throwZ;
            heaveMin = heaveMax = sample.heave;
        } else {
            sample.along = curve.samples.last().along + len(mid - prevMid);
            throwMin = std::min(throwMin, sample.throwZ);
            throwMax = std::max(throwMax, sample.throwZ);
            heaveMin = std::min(heaveMin, sample.heave);
            heaveMax = std::max(heaveMax, sample.heave);
        }
        curve.samples.append(sample);
        prevMid = mid;
    }
    curve.throwMin = throwMin;
    curve.throwMax = throwMax;
    curve.heaveMin = heaveMin;
    curve.heaveMax = heaveMax;
    curve.message = QStringLiteral("交线样本 %1，弧长 %2").arg(curve.samples.size()).arg(curve.length, 0, 'f', 3);
    return curve;
}

SectionCut intersectSurfaceWithSection(const paleo::fault::FaultSurfaceMesh &mesh, const SurveyFrame &frame,
                                       const paleo::fault::FaultSectionRef &section)
{
    SectionCut cut;
    QString why;
    if (!frameAxesOk(frame, &why)) {
        cut.status = SurfaceBuildStatus::BadFrame;
        cut.message = why;
        return cut;
    }
    Vec3 origin;
    Vec3 axis;
    if (section.kind == paleo::fault::FaultSectionRef::Inline) {
        origin = worldAt(frame, section.index, frame.xlineMin, 0);
        const Vec3 far = worldAt(frame, section.index, frame.xlineMax, 0);
        axis = far - origin;
    } else if (section.kind == paleo::fault::FaultSectionRef::Xline) {
        origin = worldAt(frame, frame.inlineMin, section.index, 0);
        const Vec3 far = worldAt(frame, frame.inlineMax, section.index, 0);
        axis = far - origin;
    } else {
        QVector<QPair<double, double>> path;
        if (!parsePath(section.pathId, &path, &why)) {
            cut.status = SurfaceBuildStatus::BadSection;
            cut.message = why;
            return cut;
        }
        origin = worldAt(frame, path.first().first, path.first().second, 0);
        const Vec3 far = worldAt(frame, path.last().first, path.last().second, 0);
        axis = far - origin;
    }
    const Vec3 horizontal{axis.x, axis.y, 0};
    if (!(len(horizontal) > 1e-9)) {
        cut.status = SurfaceBuildStatus::BadSection;
        cut.message = QStringLiteral("剖面在地图上没有长度");
        return cut;
    }
    const Vec3 normal = cross(horizontal, Vec3{0, 0, 1});
    HorizonPlane plane;
    plane.nx = normal.x;
    plane.ny = normal.y;
    plane.nz = normal.z;
    plane.d = normal.x * origin.x + normal.y * origin.y + normal.z * origin.z;
    double scale = 1;
    for (const auto &v : mesh.vertices)
        scale = std::max(scale, 1.0 + std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z));
    const QVector<Vec3> poly = chainSegments(meshPlaneSegments(mesh, plane), 1e-6 * scale);
    if (poly.size() < 2) {
        cut.status = SurfaceBuildStatus::NoIntersection;
        cut.message = QStringLiteral("断面不穿过该剖面");
        return cut;
    }
    for (const Vec3 &p : poly) {
        SectionHit hit;
        hit.x = p.x;
        hit.y = p.y;
        hit.z = p.z;
        if (!traceAndSample(frame, section, p, &hit.traceFrac, &hit.sample, &why)) {
            cut.status = SurfaceBuildStatus::BadSection;
            cut.message = why;
            cut.hits.clear();
            return cut;
        }
        cut.hits.append(hit);
    }
    cut.message = QStringLiteral("交点 %1").arg(cut.hits.size());
    return cut;
}

bool segmentIntersectsTriangles(double ax, double ay, double az, double bx, double by, double bz,
                                const std::vector<Triangle3> &tris)
{
    const Vec3 a{ax, ay, az};
    const Vec3 b{bx, by, bz};
    for (const Triangle3 &t : tris) {
        if (segmentHitsTriangle(a, b, {t.ax, t.ay, t.az}, {t.bx, t.by, t.bz}, {t.cx, t.cy, t.cz}))
            return true;
    }
    return false;
}

bool segmentIntersectsMesh(double ax, double ay, double az, double bx, double by, double bz,
                           const paleo::fault::FaultSurfaceMesh &mesh)
{
    std::vector<Triangle3> tris;
    tris.reserve(static_cast<std::size_t>(mesh.triangles.size()));
    for (const auto &t : mesh.triangles) {
        if (t.a < 0 || t.b < 0 || t.c < 0 || t.a >= mesh.vertices.size() || t.b >= mesh.vertices.size() ||
            t.c >= mesh.vertices.size())
            continue;
        const auto &va = mesh.vertices[t.a];
        const auto &vb = mesh.vertices[t.b];
        const auto &vc = mesh.vertices[t.c];
        tris.push_back(Triangle3{va.x, va.y, va.z, vb.x, vb.y, vb.z, vc.x, vc.y, vc.z});
    }
    return segmentIntersectsTriangles(ax, ay, az, bx, by, bz, tris);
}

SectionCut intersectSurfaceWithPolyline(const paleo::fault::FaultSurfaceMesh &mesh,
                                        const QVector<QPair<double, double>> &xyPolyline)
{
    SectionCut cut;
    if (xyPolyline.size() < 2) {
        cut.status = SurfaceBuildStatus::BadSection;
        cut.message = QStringLiteral("折线少于两个点");
        return cut;
    }
    // 逐折线段建竖直面切 mesh；交线段端点（=断面三角形边与面的交点）即
    // 投绘采样点，投影到折线得累计长分数 traceFrac，z 为深度。
    double scale = 1;
    for (const auto &v : mesh.vertices)
        scale = std::max(scale, 1.0 + std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z));
    const double tol = 1e-6 * scale;

    QVector<double> cumLen(xyPolyline.size(), 0.0);
    double total = 0.0;
    for (int i = 1; i < xyPolyline.size(); ++i) {
        const double dx = xyPolyline[i].first - xyPolyline[i - 1].first;
        const double dy = xyPolyline[i].second - xyPolyline[i - 1].second;
        total += std::sqrt(dx * dx + dy * dy);
        cumLen[i] = total;
    }
    if (!(total > 1e-9)) {
        cut.status = SurfaceBuildStatus::BadSection;
        cut.message = QStringLiteral("折线在地图上没有长度");
        return cut;
    }

    for (int i = 1; i < xyPolyline.size(); ++i) {
        const Vec3 a{xyPolyline[i - 1].first, xyPolyline[i - 1].second, 0.0};
        const Vec3 b{xyPolyline[i].first, xyPolyline[i].second, 0.0};
        const Vec3 horizontal{b.x - a.x, b.y - a.y, 0};
        const double segLen = len(horizontal);
        if (!(segLen > 1e-9))
            continue;
        const Vec3 normal = cross(horizontal, Vec3{0, 0, 1});
        HorizonPlane plane;
        plane.nx = normal.x;
        plane.ny = normal.y;
        plane.nz = normal.z;
        plane.d = normal.x * a.x + normal.y * a.y + normal.z * a.z;
        // 交线段裁剪到本折线段范围（井径两端截断断层线）：两端投影参数
        // 同侧在外 → 整段丢弃；否则端点钳到 [0,1] 边界（边界交点即剖面
        // 端处的断层深度）。
        for (const Seg &s : meshPlaneSegments(mesh, plane)) {
            const double tA =
                dot(Vec3{s.a.x - a.x, s.a.y - a.y, 0}, horizontal) / (segLen * segLen);
            const double tB =
                dot(Vec3{s.b.x - a.x, s.b.y - a.y, 0}, horizontal) / (segLen * segLen);
            if ((tA < 0.0 && tB < 0.0) || (tA > 1.0 && tB > 1.0))
                continue;
            for (double t : {tA, tB}) {
                const double tc = std::max(0.0, std::min(1.0, t));
                const Vec3 p = s.a + (s.b - s.a) * ((tB - tA) != 0.0
                                                        ? (tc - tA) / (tB - tA)
                                                        : 0.0);
                SectionHit hit;
                hit.traceFrac = (cumLen[i - 1] + tc * segLen) / total;
                hit.x = p.x;
                hit.y = p.y;
                hit.z = p.z;
                cut.hits.push_back(hit);
            }
        }
    }
    std::sort(cut.hits.begin(), cut.hits.end(),
              [](const SectionHit &l, const SectionHit &r) {
                  return l.traceFrac < r.traceFrac;
              });
    // 近重合点去重（相邻切口的共享端点）。
    QVector<SectionHit> dedup;
    for (const SectionHit &h : cut.hits) {
        if (!dedup.isEmpty()) {
            const SectionHit &last = dedup.back();
            if (std::fabs(last.traceFrac - h.traceFrac) * total < tol &&
                std::fabs(last.z - h.z) < tol)
                continue;
        }
        dedup.push_back(h);
    }
    cut.hits = dedup;
    if (cut.hits.size() < 2) {
        cut.status = SurfaceBuildStatus::NoIntersection;
        cut.message = QStringLiteral("断面不穿过该折线");
        cut.hits.clear();
    }
    return cut;
}

} // namespace paleo::faultsurf
