// 层：功能
#pragma once
// facies-ux 5：派生图件登记时的证据链组装（目录解析父版本 → domain 结构）。
// MappingWorkbench::record 与 CompositionWorkflow 共用；只在 owner 线程调用。
#include <QStringList>
#include <QVariantMap>

class DataCatalog;

namespace paleo::workflow_detail
{
QVariantMap evidenceRecord( const DataCatalog *catalog, const QStringList &parentVersionIds,
                            const QVariantMap &extra );
}
