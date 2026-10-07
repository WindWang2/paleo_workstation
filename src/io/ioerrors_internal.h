// 层：数据
#pragma once

// 方向70（Unity 清障）：paleo_io 的 sattrio/lasparser/horizonbinner/
// attrgridout 各带一份匿名 namespace 同构 setError，UNITY_BUILD 合批即
// redefinition。收拢为单一 inline 定义（先例
// workflow/workflowerrors_internal.h）。dataimport_internal.h 的
// dataimport_detail::setError 改为委托本头——同目标内两个命名空间各持
// 同签名 inline 时，混批 TU 的两条 using 会导入不同实体致调用点歧义，
// 全目标必须单一 canonical 定义。

#include <QString>

namespace paleo::io_detail {

/// 失败文案落点：调用方提供 error 才写，不改变调用方的返回值。
inline void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

} // namespace paleo::io_detail
