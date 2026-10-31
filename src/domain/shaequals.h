// 层：数据
#pragma once
#include <QString>

// #890：SHA-256 十六进制摘要比较的唯一口径——大小写不敏感。本仓库 toHex()
// 产小写，但外部清单/手填/旧版本可能是大写；字面 == 会假阴性（误判「内容已变」）。
// 语义同 QString::operator==，仅忽略大小写（空串仍只等于空串）。
// 放 domain（最底层、无依赖）：放 io 会令 catalog/metadata→io 成环（check_layering）。
namespace paleo::domain {
inline bool shaEquals(const QString &a, const QString &b)
{
  return a.compare(b, Qt::CaseInsensitive) == 0;
}
} // namespace paleo::domain
