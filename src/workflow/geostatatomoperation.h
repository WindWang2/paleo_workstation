// 层：功能
// workflow/geostatatomoperation.h — I1 地统计原子的 CapabilityExecutionAdapter。
//
// 复用既有 Host/Recovery/TaskService 缝（registerExecutionAdapter；与 python
// operation 族同款 capture 形态），不另建任务系统、不覆盖全局 restorer：
//   · capture（owner）：严格解码 → freezeAnalysisUnit 实体核验 →
//     resolveManagedVersionRef 解析受管样本版本 → 样本↔单元关联核验；
//   · preparation.worker（TaskService worker）：受管样本物化为内容寻址
//     私有快照（纯值 + SHA-256）并装配 FrozenOperation；
//   · preparation.validate（owner）：物化后复核绑定代次与源版本存在性；
//   · worker（数值）：组装 OrdinaryKrigingInputs / SgsInputs 调既有
//     NativeCppAdapter 核，写 attempt 私有目录，按 OUTPUT/VARIANCE 与
//     OUTPUT/STD 固定端口出成果；SGS 冻结 persistRealizations=false；
//   · restoreWorker/restoreInputValidator/validateFrozen：从持久冻结计划
//     （含私有快照凭据）重连，快照缺失/摘要不符 → 明确不可恢复。
#pragma once

#include "capabilityhost.h"
#include "geostatatomcontract.h"

#include <QJsonObject>
#include <QString>

#include <functional>

class DataCatalog;

namespace paleo::geostat_atom
{

// 组装根注入的受控 provider（owner 线程访问；capture/validate 在 owner 跑）。
struct GeostatAtomProviders
{
  std::function<bool()> alive;             // 绑定代次仍有效（组装根复核）
  std::function<DataCatalog *()> catalog;  // 当前 catalog（可空）
};

// 为单一 capabilityId（geostat.kriging / geostat.sgs）构建执行适配器。
paleo::capability::CapabilityExecutionAdapter geostatAtomExecutionAdapter(
    const QString &capabilityId, const GeostatAtomProviders &providers );

// 聊天回包上的有界冻结计划事实。prepare 阶段不运行数值 worker、记录没有
// 摘要，物化成本/来源/单位/有效参数只在持久计划里——这里按 Recovery 同款
// 授权口径（同文档、同调用方作用域的 schema-v2 记录）读回并投影为
// payload.frozenPlan（不含路径）。不可读或非地统计记录时原样返回；
// 追加后重新过 reply 预算。documentId/callerScope 须在宿主存活时取值。
QJsonObject geostatAtomToolResponse( QJsonObject payload, const paleo::capability::ExecuteResult &result,
                                     const QString &projectDir, const QString &operationDir,
                                     const QString &documentId, const QString &callerScope );

} // namespace paleo::geostat_atom
