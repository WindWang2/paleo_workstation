// 层：功能
#pragma once

#include <QString>

namespace paleo::workflow_detail {

/// 失败文案落点：调用方提供 error 才写，不改变调用方的返回值。
inline void setError( QString *error, const QString &text )
{
  if ( error )
    *error = text;
}

} // namespace paleo::workflow_detail
