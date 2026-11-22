// 层：功能
#include "operationprivatedir.h"
#include "metadata/managedpath.h"
#include "metadata/operationnamespace.h"

#include <QDir>
#include <QFileInfo>

namespace paleo::capability::operation_detail {
namespace {
using operation_namespace::isCanonicalAttemptId;
using operation_namespace::isCanonicalOperationId;
using operation_namespace::isCanonicalResultLeaf;

// 校验所有已存在段：任一段是符号链接/非目录/越出工程根即拒绝。
// 未存在的尾段交给调用方的 mkpath（父子段一并创建）。
bool walkExistingSegments(const QString &root, const QStringList &segments, QString &code)
{
  QString current = root;
  for (const QString &segment : segments)
  {
    current = QDir(current).filePath(segment);
    const QFileInfo info(current);
    if (info.isSymbolicLink())
    {
      code = QStringLiteral("unsafe_operation_directory");
      return false;
    }
    if (info.exists())
    {
      // canonicalFilePath 恒以 '/' 分隔（含 Windows），且以磁盘真实大小写返回。
      if (!info.isDir() || !info.canonicalFilePath().startsWith(root + QLatin1Char('/')))
      {
        code = QStringLiteral("unsafe_operation_directory");
        return false;
      }
    }
  }
  return true;
}
} // namespace

PrivateDir createPrivateAttemptDir(const QString &projectDir, const QString &operationId,
                                   const QString &attemptId, const QString &leaf)
{
  PrivateDir out;
  if (!isCanonicalOperationId(operationId) || !isCanonicalAttemptId(attemptId) ||
      (!leaf.isEmpty() && !isCanonicalResultLeaf(leaf)))
  {
    out.code = QStringLiteral("invalid_operation_identity");
    return out;
  }
  const QString root = QFileInfo(projectDir).canonicalFilePath();
  if (root.isEmpty() || !QFileInfo(root).isDir())
  {
    out.code = QStringLiteral("operation_root_unavailable");
    return out;
  }
  QStringList segments{QStringLiteral(".paleo"), QStringLiteral("capability-ops"), operationId, attemptId};
  if (!leaf.isEmpty())
    segments.append(leaf);
  if (!walkExistingSegments(root, segments, out.code))
    return out;
  const QString expected = QDir(root).filePath(segments.join(QLatin1Char('/')));
  // 幂等：目录已完整存在则直接进入复核，不重复创建。
  if (!QFileInfo::exists(expected) && !QDir().mkpath(expected))
  {
    // 与身份校验/根不可用区分：这里是与输入无关的文件系统/权限失败。
    out.code = QStringLiteral("stage_failed");
    return out;
  }
  const QFileInfo created(expected);
  // 创建后复核：真实目录、无链接别名、规范路径仍在工程根内。根取自磁盘
  // 规范化路径，Windows 大小写差异不会误伤；越出根即说明创建链被外部
  // 篡改（残余 TOCTOU 窗口收敛到本复核点之后）。
  if (!created.isDir() || created.isSymbolicLink() ||
      !created.canonicalFilePath().startsWith(root + QLatin1Char('/')))
  {
    out.code = QStringLiteral("unsafe_operation_directory");
    return out;
  }
  out.path = QDir::cleanPath(expected);
  return out;
}
} // namespace paleo::capability::operation_detail
