// 层：数据
#pragma once

#include "../singlefactor/fieldcontours.h"
#include "../singlefactor/types.h"

#include <QVariantMap>

#include <string>
#include <vector>

// faciesmapping/candidateboundaries — 候选相界提取（goal/facies-automapping
// 阶段2）。单因素等值线（field_contours 语义：level → polylines）与约束相区
// 求交/裁剪 → 候选相边界多边形，带来源证据标记（哪条等值线/约束贡献）。
// GEOS 全程走 singlefactor/geosutil 的 RAII 句柄；无 QtWidgets、无文件发布。

namespace paleo::faciesmapping
{

using singlefactor::ContourLevelLines;
using singlefactor::Control;
using singlefactor::Point2;
using singlefactor::Polygon;
using singlefactor::Status;

// 约束相区（ConstraintStore 的多边形约束：id + facies_code 口径）。
struct FaciesZone
{
  std::string id;
  int faciesCode = -1;  // -1 = 区内相未定（仍参与边界构建，不赋相）
  std::vector<Polygon> polygons;
};

// 候选编图单元：一个面 × 一个相归属（可能未定）。
struct CandidateRegion
{
  std::string regionId;  // "r0","r1",… 发射序稳定（QA/合成定位用）
  int faciesCode = -1;   // 来自约束区；-1 = 无约束证据（未定相，交合成阶段裁决）
  Polygon geometry;
  double area = 0;
  // 证据标记："level:<L>#<seg>"（L=等值线级别，seg=该级别内线序）。
  std::vector<std::string> contourEvidence;
  std::vector<std::string> constraintEvidence;  // 贡献边界的约束区 id
  bool touchesDomainEdge = false;
};

struct CandidateBoundaryRequest
{
  std::vector<Polygon> domain;                // 编图域；空 = 等值线自闭合（无外壳）
  std::vector<ContourLevelLines> contours;    // 升序级别（field_contours 语义）
  std::vector<FaciesZone> zones;              // 约束相区（裁剪到域）
  double minArea = 0;                         // 面积 < minArea 的碎片丢弃（<=0 不丢）
  double snapTolerance = 1e-9;                // 线段求交/接触判定的几何容差
};

struct CandidateBoundaryResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<CandidateRegion> regions;
  QVariantMap diagnostics;  // face_count/dropped_small/zone_conflict_faces/…
};

// 几何管线：域并集 → 等值线裁剪到域 →（域环+等值线）unaryUnion 节点化 →
// polygonize → 面 × 约束区求交 → 候选单元。等值线不闭合且无域时会产生零面
//（如实返回 Ok + 诊断说明，不伪造闭合）。
CandidateBoundaryResult extractCandidateRegions( const CandidateBoundaryRequest &request,
                                                 const Control *control = nullptr );

} // namespace paleo::faciesmapping
