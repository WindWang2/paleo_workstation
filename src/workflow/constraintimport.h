// 层：功能
#pragma once

#include <QString>
#include <functional>
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
// 调用侧需要按原因分流时读 failure 码，不匹配 tr() 译文——译文依赖两个
// 翻译上下文（constraintimport 与调用方）保持子串包含关系，启用翻译后
// 静默失配（finding-143）。
enum class ConstraintImportFailure {
  None,              // 未失败，或非下列可判定原因（错误详情看 error 文本）
  RoleUndetermined,  // role=auto：字段特征与文件名提示均无法判定方向线/打断线
};

QVector<ImportedConstraintRecord>
readConstraintImportFeatures(const QString &path, const QString &role, QString *resolvedRole,
                             QString *error, const std::function<bool()> &cancelled = {},
                             ConstraintImportFailure *failure = nullptr);

} // namespace paleo
