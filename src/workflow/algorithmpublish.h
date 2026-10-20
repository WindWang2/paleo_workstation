// 层：功能
// workflow/algorithmpublish.h — algo-atom P1：统一算法产出发布面。
// 算法核 / Processing 不算 catalog；workflow 在 prepare 已 stage 后，把
// AlgorithmRunResult 经 DerivedAssetRegistrar::commitExternal 登记 DERIVED。
// 设计：docs/designs/algorithm-atom-contract.md §3 / P1。
#pragma once

#include "derivedassets.h"

#include "../services/algorithmspec.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace paleo
{

// 从 Processing 结果 map 组装最小 RunResult：ok + 单件 OUTPUT 工件。
// 空/无路径 → ok=false（error 可空，由调用方补人类可读原因）。
AlgorithmRunResult runResultFromProcessingOutput(
    const QVariantMap &results, const QString &portId = QStringLiteral( "OUTPUT" ) );

// 将 RunResult 中目的地型工件发布为 DERIVED 版本。
// · staging：prepare 阶段已 stage 的受管落位
// · parentVersionIds / sourceUri / extra / fingerprint：与 commitExternal 同语义
// · 忽略 kind==string 的诊断端口；优先 portId==OUTPUT 的工件路径
// · 行为等价于 registrar.commitExternal(staging, artifactPath, …)
bool publishArtifacts( DerivedAssetRegistrar &reg, const DerivedStaging &staging,
                       const AlgorithmRunResult &result,
                       const QStringList &parentVersionIds, const QString &sourceUri,
                       const QVariantMap &extra, QString *error = nullptr,
                       const DerivedFileFingerprint *fingerprint = nullptr );

} // namespace paleo
