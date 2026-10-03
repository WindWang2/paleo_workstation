// 层：功能
#pragma once

#include <QString>
#include <QVariantMap>
#include <QVector>

// 上游 _extract_current_directions / _extract_current_barriers 移植：
// 约束矢量文件（SHP/GPKG/GeoJSON）→ ConstraintStore 记录（WKT + type +
// params 键值），属性解析复用 wellacquisition 的上游规则。
// role: "direction" | "barrier" | "auto"（auto 先按字段特征后按文件名判定）。

namespace paleo
{

struct ImportedConstraintRecord
{
  QString wkt;
  QString type;        // ConstraintStore 存储类型：direction_line / break_line
  QVariantMap params;  // lineParamsJson 键（semantic/enabled/ratio/…/blockMode）
};

// 解析失败返回空列表并写 error；role=auto 且无法判定时 error 提示用户选择。
QVector<ImportedConstraintRecord> readConstraintImportFeatures( const QString &path,
                                                                const QString &role,
                                                                QString *resolvedRole,
                                                                QString *error );

} // namespace paleo
