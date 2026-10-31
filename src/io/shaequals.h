// 层：数据
#pragma once
#include <QString>

// #890：SHA-256 十六进制摘要比较的唯一口径——大小写不敏感。本仓库 toHex()
// 产小写，但外部清单/手填/旧版本可能是大写；字面 == 会假阴性（误判「内容已变」）。
// 语义同 QString::operator==，仅忽略大小写（空串仍只等于空串）。
namespace paleo::io {
inline bool shaEquals(const QString &a, const QString &b)
{
  return a.compare(b, Qt::CaseInsensitive) == 0;
}
} // namespace paleo::io
