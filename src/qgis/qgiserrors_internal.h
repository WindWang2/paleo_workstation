// 层：QGIS 封装
#pragma once

// 方向70（Unity 清障）：paleo_qgis 8 个 .cpp 匿名 namespace 各带一份同构
// setError，UNITY_BUILD 合批即 redefinition。收拢为单一 inline 定义
//（先例 workflow/workflowerrors_internal.h）；各 .cpp 匿名 namespace 顶部
// `using paleo::qgis_detail::setError;` 接线，调用点零改动。

#include <QString>

namespace paleo::qgis_detail {

/// 失败文案落点：调用方提供 error 才写，不改变调用方的返回值。
inline void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

} // namespace paleo::qgis_detail
