// 层：数据
#pragma once

// 方向70（Unity 清障）：paleo_store（metadata×catalog 同库互依环）原先每
// 文件匿名 namespace 各带一份同构 setError，UNITY_BUILD 合批即 redefinition。
// 收拢为单一 inline 定义（先例 workflow/workflowerrors_internal.h）；
// 各 .cpp 匿名 namespace 顶部 `using paleo::store_detail::setError;` 接线，
// 调用点零改动。catalog 侧经 ../metadata/ 引用（同库互依，先例
// catalogstore.cpp -> ../metadata/atomicfile.h）。

#include <QString>

namespace paleo::store_detail {

/// 失败文案落点：调用方提供 error 才写，不改变调用方的返回值。
inline void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

} // namespace paleo::store_detail
