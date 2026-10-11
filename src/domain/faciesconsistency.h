// 层：数据
#pragma once
// facies-ux 4：地震相 × 测井相预测一致性报告（纯数据）。
//
// 输入：逐井样本（井、测井相编码、该井位处的地震相编码；任一缺失计入
// 「未比对」）与相分类表。输出：逐井一致性、混淆矩阵（行 = 测井相，
// 列 = 地震相）、总体一致率与 Cohen κ。编码按整数比较——同一层位的两类
// 预测共用该层位相分类，名称/颜色只用于展示。
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <optional>

namespace paleo::facies
{
struct ConsistencySample
{
  QString wellId;
  QString wellName;
  double x = 0.0;
  double y = 0.0;
  std::optional<int> wellCode;    // 测井相（未预测 = 空）
  std::optional<int> seismicCode; // 井位处的地震相（井在图外/nodata = 空）
};

struct ConsistencyReport
{
  struct Row
  {
    ConsistencySample sample;
    bool compared = false; // 两侧编码都有
    bool agree = false;
    QString reason;        // 未比对原因
  };
  QVector<Row> rows;
  QVector<int> codes;               // 矩阵轴（两侧出现过的编码 ∪ 分类编码，升序）
  QVector<QVector<int>> confusion;  // [测井相][地震相]
  int compared = 0;
  int agreed = 0;
  double agreement = 0.0;           // agreed / compared；compared = 0 时 0
  double kappa = 0.0;               // Cohen κ；期望一致率 = 1 时记 1（全同类）
  QVariantMap toVariant( const QVariantList &schema ) const;
};

ConsistencyReport computeConsistency( const QVector<ConsistencySample> &samples,
                                      const QVariantList &schema );
} // namespace paleo::facies
