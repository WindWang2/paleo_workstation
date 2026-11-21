// 层：数据
#include "operationnamespace.h"

#include <QUuid>

namespace paleo::capability::operation_namespace
{
namespace {
// QUuid::toString(WithoutBraces) 的输出恒为小写十六进制；任何大写、花括号、
// urn 前缀、缺连字符或非规范连字符位置都在此被拒——不 trim、不归一化、
// 不改写后继续。（review R1：连字符位置必须强制为 '-'，十六进制字符不能
// 占据连字符位，否则无连字符的 36 位 hex 串会被当作规范身份。）
bool isLowercaseUuid(QStringView id)
{
  static const QString pattern = QUuid().toString(QUuid::WithoutBraces); // 仅取长度参照
  if (id.size() != pattern.size())
    return false;
  for (int i = 0; i < id.size(); ++i)
  {
    const QChar c = id.at(i);
    const bool dashPosition = i == 8 || i == 13 || i == 18 || i == 23;
    if (dashPosition)
    {
      if (c != QLatin1Char('-'))
        return false;
    }
    else if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
               (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
      return false;
  }
  return true;
}
bool startsWithLowercaseUuid(QStringView id, int prefixLength)
{
  return id.size() == prefixLength + 36 && isLowercaseUuid(id.mid(prefixLength));
}
} // namespace

bool isCanonicalOperationId(QStringView id)
{
  return id.size() == 3 + 36 && id.startsWith(QLatin1String("op-")) &&
      startsWithLowercaseUuid(id, 3);
}

bool isCanonicalAttemptId(QStringView id)
{
  if (isLowercaseUuid(id))
    return true;
  return id.size() == 8 + 36 && id.startsWith(QLatin1String("attempt-")) &&
      startsWithLowercaseUuid(id, 8);
}

bool isCanonicalResultLeaf(QStringView leaf)
{
  return leaf.size() == 7 + 36 && leaf.startsWith(QLatin1String("result-")) &&
      startsWithLowercaseUuid(leaf, 7);
}

QString privateOperationPrefix(const QString &operationId)
{
  if (!isCanonicalOperationId(operationId))
    return {};
  return QStringLiteral(".paleo/capability-ops/") + operationId + QLatin1Char('/');
}

bool isInPrivateOperationNamespace(QStringView relative, QStringView operationId)
{
  if (!isCanonicalOperationId(operationId))
    return false;
  // ".paleo/capability-ops/" = 22 字符；操作段后必须紧跟 '/' 且至少还有
  // 一个内容字符（review R1：裸命名空间前缀本身不算成员路径）。
  if (relative.size() < operationId.size() + 24)
    return false;
  if (!relative.startsWith(QLatin1String(".paleo/capability-ops/")))
    return false;
  if (relative.sliced(22, operationId.size()) != operationId)
    return false;
  return relative.at(22 + operationId.size()) == QLatin1Char('/');
}
} // namespace paleo::capability::operation_namespace
