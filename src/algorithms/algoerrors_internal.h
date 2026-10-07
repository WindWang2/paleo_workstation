// 层：数据
#pragma once

// 方向70（Unity 清障）：stratgrid 6 个 .cpp 匿名 namespace 各带一份同构
// setError，UNITY_BUILD 合批即 redefinition。收拢为单一 inline 定义
//（先例 workflow/workflowerrors_internal.h）；各 .cpp 匿名 namespace 顶部
// `using paleo::algo_detail::setError;` 接线，调用点零改动。放 algorithms
// 根而非 stratgrid/：错误面 helper 是模块级关切（先例
// welldistance_internal.h 也在根）。

#include <QString>

namespace paleo::algo_detail {

/// 失败文案落点：调用方提供 error 才写，不改变调用方的返回值。
inline void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

} // namespace paleo::algo_detail
