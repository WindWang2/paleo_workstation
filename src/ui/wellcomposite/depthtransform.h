// 层：视图
#pragma once

#include <QString>
#include <QVector>

#include <QPair>

#include <optional>

#include "domain/deviationsurvey.h"
#include "domain/wellcompositemodel.h"

// ui/wellcomposite/depthtransform — D6.x 深度变换与查询
//
//   D6.1 MD↔TVD：井斜测量表存在时最小曲率法换算；缺表禁用并给原因
//   D6.2 海拔基准换算：补心高程 KB（TVDSS = KB − TVD）
//   D6.3 深度单位 m/ft：显示层换算（换算函数在 depthtools.h）
//   D6.4 按深度取样查询 API（tooltip/统计用）
//   D6.5 TWT 显示：井有时深表时并列换算
//   D6.6 深度采样重抽：大图缩放 LOD 抽稀（min-max 桶抽稀保极值）与恢复

namespace WellComposite
{

// 井斜测量站（测点 MD、井斜角°、方位角°）
struct DeviationStation
{
  double md = 0.0;
  double inclinationDeg = 0.0;
  double azimuthDeg = 0.0;
};

class DepthTransform
{
public:
  // ---- D6.1 MD→TVD ----
  void setDeviationSurvey(const QVector<DeviationStation> &stations);
  bool hasDeviationSurvey() const { return m_survey.has_value(); }
  // 缺表禁用原因（tooltip/禁用态文案）
  QString deviationUnavailableReason() const;
  double mdToTvd(double md) const;      // 域模型最小曲率（站间子段 + 端站姿态外延）
  double tvdToMd(double tvd) const;     // 同一正函数的数值反解

  // ---- D6.2 海拔基准 ----
  void setKbElevation(double kbMeters); // 补心高程（m，海拔基准）
  bool hasKbElevation() const { return m_hasKb; }
  double kbElevation() const { return m_kb; }
  double mdToTvdss(double md) const;    // TVDSS = KB − TVD
  double elevationAt(double md) const { return mdToTvdss(md); }

  // ---- D6.5 TWT ----
  // pairs: (TVD m, TWT ms)；存在时启用并列显示
  void setTimeDepthTable(const QVector<QPair<double, double>> &tvdTwtPairs);
  bool hasTimeDepthTable() const { return !m_twtStations.isEmpty(); }
  QString twtUnavailableReason() const;
  double twtAtTvd(double tvd) const;

  // ---- D6.4 按深度取样 ----
  struct DepthSample
  {
    double depth = 0.0;
    float value = 0.0f;
    bool valid = false;
  };
  static DepthSample sampleAt(const CurveData &curve, double depth);
  // 区间取样（等步长；供统计/绘图重采样）
  static QVector<DepthSample> sampleRange(const CurveData &curve, double fromDepth,
                                          double toDepth, double stepDepth);

  // ---- D6.6 LOD 抽稀 ----
  // min-max 桶抽稀：把 (depths, values) 压到 ≤ targetPoints 点，每桶保留
  // 首末点与桶内极值点（视觉保真），返回 (depth, value) 对序列；targetPoints
  // ≥ 原点数时原样返回（恢复 = 直接用原始数组）。
  static QVector<QPair<float, float>> decimateForLod(const QVector<float> &depths,
                                                     const QVector<float> &values,
                                                     int targetPoints);
  // 抽稀是否值得（点数超阈值才启用）
  static bool shouldDecimate(int pointCount, int targetPoints)
  {
    return pointCount > targetPoints * 2;
  }

private:
  // goal/well-trajectory：MD↔TVD 统一走域模型最小曲率（全狗腿含方位）。
  // 站表无效（重复 MD/越界角等）→ 保持在禁用态并记原因（面板如实显示）。
  std::optional<paleo::WellDeviationSurvey> m_survey;
  QString m_deviationInvalidReason;
  QVector<QPair<double, double>> m_twtStations;
  bool m_hasKb = false;
  double m_kb = 0.0;
};

} // namespace WellComposite
