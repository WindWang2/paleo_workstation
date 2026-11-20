// 层：功能
#pragma once
#include "../catalog/datacatalog.h"
#include "derivedassets.h"

// workflow/predictionpublication — P4-W6 预测发布协调（任务私有）。
//
// 生产预测发布链的持久凭证事实核对。此前 publishPredictionJob 只依赖
// publishArtifacts 的 bool 返回：重复回调撞 duplicate version id、崩溃后
// 重试无从判断「是否已提交」。本组件提供最小协调——按 staging.versionId
// 做「可证明的提交事实」查询，不复制 CapabilityRecovery/commitSet 协议、
// 不另建全局发布数据库、不升级收据 schema：
//   · Committed —— versionId 已在 catalog，且其受管文件在盘、sha256 与
//     catalog 记录一致（文件级证据，不依赖内存状态）；
//   · NotCommitted —— catalog 可查而 versionId 不存在（真实未提交）；
//   · CommitStateUnknown —— catalog 不可用/文件证据矛盾——调用方必须阻断，
//     不能把查询失败当作未提交（那样会重复推理/重复提交）。
namespace paleo::workflow
{

enum class PredictionCommitFact
{
  NotCommitted,
  Committed,
  CommitStateUnknown,
};

struct PredictionCommitEvidence
{
  PredictionCommitFact fact = PredictionCommitFact::CommitStateUnknown;
  CatalogVersion version;   // Committed 时为完整版本记录
  QString detail;           // 证据/错误描述（如实进错误链）
  bool provable() const { return fact != PredictionCommitFact::CommitStateUnknown; }
};

PredictionCommitEvidence queryPredictionCommit( DataCatalog *catalog, const DerivedStaging &staging );

} // namespace paleo::workflow
