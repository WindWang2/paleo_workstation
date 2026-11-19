// 层：功能
#pragma once

#include <QString>
#include <QStringList>

#include "domain/stratigraphicunit.h"

class DataCatalog;
class CatalogVersion;

// workflow/ — 单因素/约束插值成果的「证据确认」收口（P3/W6、W7）。
//
// 把一次发布的完成事实拆成可独立核验的状态：
//   · 数值完成 + 受管成果提交 —— DerivedAssetRegistrar::commit/commitSet 的
//     持久收据（既有事实源，本组件不重造）；
//   · 权威证据确认 —— Data Service 中存在 id="catalog.<versionId>" 的
//     SubIntervalResultBinding（绑定即事实，不以同步函数返回值充当确认）。
//
// 确认复用既有投影器 syncAnalysisResults（幂等、按绑定 id 去重），本组件只做：
// 目标绑定存在性核验、未完成步骤持久化（版本 extra.evidence_pending，与
// stale 投影同一 updateVersionExtra 接缝）、以及按版本的定向重试。
// 不声称文件/Catalog/Data Service 跨存储 ACID——提交后失败保留已提交产物，
// 持久化剩余步骤供恢复。
namespace paleo::factor_evidence
{

struct Confirmation
{
  bool catalogCommitted = false; // 受管版本已提交（versionId 非空即真）
  bool evidenceConfirmed = false; // 目标绑定确实存在
  QString versionId;
  QString bindingId;   // 期望的稳定绑定 id（catalog.<versionId>）
  QString pendingReason; // 未确认的精确原因（空=已确认）
};

// 校验某次已提交成果的权威证据：先跑既有幂等投影，再查目标绑定是否存在。
// 确认成功清除该版本 extra.evidence_pending；失败写入 {step, error, unitKey}
// 的持久记录（不动血缘/收据字段）。catalog 不可写时只读返回状态。
Confirmation confirmVersionEvidence( DataCatalog *catalog, const QString &projectDir,
                                     const QString &versionId, const paleo::strat::UnitSelection &unit );

// 列出带 evidence_pending 标记的版本（恢复工作清单；绑定本身仍是权威事实）。
QStringList pendingEvidenceVersions( DataCatalog *catalog );

// 定向恢复/重试：逐个确认 pending 版本，只补「投影 + 绑定核验 + 清标记」，
// 不重算数值、不新建版本、不重复登记（绑定 id 主键幂等）。返回仍待确认数。
int retryPendingEvidence( DataCatalog *catalog, const QString &projectDir, QString *error = nullptr );

} // namespace paleo::factor_evidence
