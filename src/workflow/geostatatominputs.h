// 层：功能
// workflow/geostatatominputs.h — I1 受管点样本的后台物化与私有快照。
//
// 物化发生在 TaskService worker（CapabilityHost capture 的 preparation.worker）：
// owner 已把受管版本解析成纯值（绝对路径 + 指纹 + 单元/因子身份），worker
// 自建有界读取器（qgis/geostatsamplereader）读点、计数 skipped、写私有快照
// 并记录 SHA-256。快照是 compute/restore 的数据身份：文件缺失或摘要不符
// → 明确不可恢复（geostat_snapshot_unavailable），不用模型重传参数重建。
//
// 存放位置：.paleo/capability-ops/geostat-snapshots/<sha256>/samples.json
//（工程内、内容寻址：同一受管版本+字段的快照天然去重；prepare 阶段尚不知
// operationId，故不能落在 per-op 私有目录）。冻结计划以
// plan.sourceVerification{path,versionId} 引用——operation retention 已登记的
// 形态，快照目录因此成为 GC 根并随操作记录保留（与 inputs/、well-inputs/
// 共享输入束同生命周期：尚无孤儿回收，如实记录）。
#pragma once

#include "geostatatomcontract.h"

#include "algorithms/geostat/types.h"

#include <QString>

#include <functional>
#include <vector>

namespace paleo::geostat_atom
{

// 物化产物：纯值样本 + 快照凭据（capture worker → 冻结操作闭包直接持有）。
struct GeostatMaterializedSamples
{
  bool ok = false;
  QString code;
  QString message;
  std::vector<paleo::geostat::Sample> samples;
  QString snapshotPath;    // 工程相对
  QString snapshotSha256;  // 快照文件 SHA-256（= 快照目录名）
  int sampleCount = 0;
  int skippedRows = 0;
  QString crsWkt;      // 样本 CRS 规范 WKT（全文只进快照，不进冻结计划/回包）
  QString crsAuthId;   // 如 EPSG:3857；本地工程网格可空
  QString crsWktSha256;
};

// 把已解析的受管样本文件物化为私有快照。sourcePath：受管版本文件绝对路径；
// versionId/versionSha256：宿主解析的受管身份（记录进快照头，恢复时对账）；
// factorId/field：因子与数值字段。
GeostatMaterializedSamples materializeGeostatSampleSnapshot(
    const QString &projectDir, const QString &sourcePath, const QString &versionId,
    const QString &versionSha256, const QString &factorId, const QString &field,
    const std::function<bool()> &cancelled );

// 从快照恢复样本（restore 路径）：形状、路径、摘要、头字段逐项对账；任何
// 不符 → geostat_snapshot_unavailable（不回退到当前样本）。
struct GeostatRestoredSamples
{
  bool ok = false;
  QString code;
  std::vector<paleo::geostat::Sample> samples;
  QString crsWkt;
  QString crsAuthId;
  QString crsWktSha256;
};
GeostatRestoredSamples restoreGeostatSampleSnapshot( const QString &projectDir,
                                                     const GeostatSamplePlan &plan );

// 快照校验（inputValidator：形状 + 安全路径 + 摘要一致，无 catalog 访问）。
QString verifyGeostatSnapshotFile( const QString &projectDir, const GeostatSamplePlan &plan );

} // namespace paleo::geostat_atom
