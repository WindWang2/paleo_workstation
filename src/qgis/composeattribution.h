// 层：QGIS 封装
#pragma once
#include <QList>
#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <qgsgeometry.h>
#include <qgsrectangle.h>
class QgsMapLayer;

// 图斑级综合编图归因：用成果保存时冻结的输入版本，按需重放确定性决策
// （priority-first-valid / threshold-classification / categorical-nearest）。
// 只重放、不重算——与 MappingArtifactWriter::composeRaster 的像元语义逐条
// 对齐（首个栅格输入定网格；胜者不覆盖；连续输入按阈值分相；相点取最近邻
// 且绝不插值数值编码）。网格不对齐或来源缺失时如实返回 unresolved，不用
// 当前版本静默替换历史输入。
namespace paleo::attribution {

// 已解析的冻结输入：versionId 为身份，layer 为按冻结路径打开的独立图层
// （属主线程；CRS 已按冻结口径设置）。missing=true 时 layer 为空并携带原因。
struct ResolvedInput {
  QString layerId, versionId, title, assetId;
  int priority = 0;
  bool continuous = false;
  QgsMapLayer *layer = nullptr;
  bool missing = false;
  QString missingReason;
};

struct ReplayRequest {
  // 目标图斑（空几何 = 整幅网格统计，用于成果级回执核对）。
  QgsGeometry polygon;
  // 局部位置级（Level 3）：在图斑内指定点重放单点决策。
  bool usePoint = false;
  QPointF point;
  int faciesCode = 0;              // 图斑当前相编码（用于一致性对比）
  int maxSampledCells = 4096;      // 采样上限（按行主序等步长抽样）
};

// 返回值对象（进 featureEvidence 快照与测试断言）：
//   status: "ok" | "unresolved" | "manual"
//   inputs[]: 每输入 {layer_id,version_id,priority,decision,cells,fraction,
//                     value_min,value_max,value_mean}
//   code_distribution[] / sampled_cells / sampling / unresolved[] / message
QVariantMap replay(const QList<ResolvedInput> &inputs,
                   const QVariantList &facies, const QList<double> &thresholds,
                   const ReplayRequest &request, QString *error);

// 单点决策（Level 3 查询与影响分析复用）：返回该点的胜出输入与决策细节；
// 无任何输入给出有效值时 decision 为空并填 unresolved 原因。
QVariantMap replayAt(const QList<ResolvedInput> &inputs,
                     const QVariantList &facies, const QList<double> &thresholds,
                     const QPointF &point, QString *error);

// 全网格重放（空间影响分析）：逐像元重放决策，供新旧输入版本像元级 diff。
// codes/winners 与网格行主序同长；winner=-1 表示该像元无输入给出有效值。
// maxCells 为网格像元上限（超过返回 invalid——调用方应退化为潜在影响，
// 不允许无界扫描）。网格规则与 replay/composeRaster 一致。
struct GridReplay {
  QVector<int> codes, winners;
  int cols = 0, rows = 0;
  QgsRectangle extent;
  bool valid = false;
};
GridReplay replayGrid(const QList<ResolvedInput> &inputs,
                      const QVariantList &facies,
                      const QList<double> &thresholds, qint64 maxCells);
} // namespace paleo::attribution
