// 层：数据
#include "cataloghealth.h"

#include "../catalog/datacatalog.h"

#include <QFileInfo>

namespace paleo::health
{

namespace
{

HealthIssue makeIssue(IssueKind kind, const QString &subject, const QString &detail)
{
  HealthIssue i;
  i.kind = kind;
  i.subject = subject;
  i.detail = detail;
  return i;
}

} // namespace

HealthReport buildCatalogHealth(DataCatalog *cat, const QString &projectDir,
                                QString *error)
{
  HealthReport report;
  if (!cat || !cat->isOpen())
  {
    if (error)
      *error = QStringLiteral("工程 catalog 未打开，无法体检");
    return report;
  }

  // ---- 缺失文件（版本级扫描，含历史版本）----
  for (const CatalogVersion &v : cat->versions())
  {
    const CatalogAsset a = cat->assetById(v.assetId);
    const QString subject = a.displayName.isEmpty() ? v.assetId : a.displayName;

    QString abs;
    if (v.managed)
    {
      abs = DataCatalog::resolvedVersionPath(projectDir, v);
      if (abs.isEmpty())
      {
        HealthIssue i = makeIssue(IssueKind::MissingFile, subject,
                                  QStringLiteral("受管路径非法：stage=%1 path=%2")
                                      .arg(v.stage, v.path));
        i.assetId = v.assetId;
        i.versionId = v.id;
        report.issues.append(i);
        continue;
      }
    }
    else
    {
      abs = v.path;
      if (abs.isEmpty())
      {
        HealthIssue i = makeIssue(IssueKind::MissingFile, subject,
                                  QStringLiteral("外链版本无源路径"));
        i.assetId = v.assetId;
        i.versionId = v.id;
        report.issues.append(i);
        continue;
      }
    }
    if (!QFileInfo(abs).exists())
    {
      HealthIssue i = makeIssue(IssueKind::MissingFile, subject, abs);
      i.assetId = v.assetId;
      i.versionId = v.id;
      report.issues.append(i);
    }
  }

  // ---- 未决链接 ----
  for (const EntityAssetLink &l : cat->unresolvedLinks())
  {
    const CatalogAsset a = cat->assetById(l.assetId);
    HealthIssue i = makeIssue(IssueKind::PendingLink,
                              a.displayName.isEmpty() ? l.assetId : a.displayName,
                              l.note);
    i.assetId = l.assetId;
    report.issues.append(i);
  }

  // ---- 角色词表违例链接 ----
  for (const EntityAssetLink &l : cat->invalidRoleLinks())
  {
    const CatalogAsset a = cat->assetById(l.assetId);
    HealthIssue i = makeIssue(
        IssueKind::InvalidRoleLink, a.displayName.isEmpty() ? l.assetId : a.displayName,
        QStringLiteral("role=%1 entity=%2 %3").arg(l.role, l.entityType, l.note));
    i.assetId = l.assetId;
    report.issues.append(i);
  }

  // ---- 孤立实体（零链接）----
  for (const CatalogEntity &e : cat->entities())
  {
    if (cat->linksForEntity(e.id).isEmpty())
    {
      HealthIssue i = makeIssue(IssueKind::OrphanEntity,
                                e.name.isEmpty() ? e.id : e.name,
                                QStringLiteral("entityType=%1").arg(e.entityType));
      i.entityId = e.id;
      report.issues.append(i);
    }
  }

  // ---- 无版本资产 ----
  for (const CatalogAsset &a : cat->assets())
  {
    if (cat->versionsForAsset(a.id).isEmpty())
    {
      HealthIssue i = makeIssue(IssueKind::NoVersionAsset,
                                a.displayName.isEmpty() ? a.id : a.displayName,
                                QStringLiteral("type=%1").arg(a.type));
      i.assetId = a.id;
      report.issues.append(i);
    }
  }

  return report;
}

QVector<HealthIssue> verifyExternalShas(
    const QVector<CatalogVersion> &versions,
    const std::function<bool(int, int, const QString &)> &progress)
{
  QVector<HealthIssue> issues;
  QVector<CatalogVersion> targets;
  for (const CatalogVersion &v : versions)
    if (!v.managed && !v.sha256.isEmpty() && !v.path.isEmpty())
      targets.append(v);

  int done = 0;
  for (const CatalogVersion &v : targets)
  {
    ++done;
    if (progress && !progress(done, targets.size(),
                              v.fileName.isEmpty() ? v.path : v.fileName))
    {
      // 取消：已发现问题保留，剩余不再碰——调用方据中断回调自标 incomplete。
      break;
    }
    QFileInfo fi(v.path);
    if (!fi.exists())
      continue; // 缺文件已由快速面报——SHA 段不重复计
    QString serr;
    const QString actual = DataCatalog::sha256FileHex(v.path, &serr);
    if (actual.isEmpty())
    {
      HealthIssue i = makeIssue(IssueKind::ShaMismatch,
                                v.fileName.isEmpty() ? v.path : v.fileName,
                                QStringLiteral("摘要计算失败：%1").arg(serr));
      i.assetId = v.assetId;
      i.versionId = v.id;
      i.sizeBytes = fi.size();
      issues.append(i);
      continue;
    }
    if (actual != v.sha256)
    {
      HealthIssue i = makeIssue(
          IssueKind::ShaMismatch, v.fileName.isEmpty() ? v.path : v.fileName,
          QStringLiteral("源文件与入库时的 SHA-256 不一致（%1 ≠ 留底 %2）")
              .arg(actual, v.sha256));
      i.assetId = v.assetId;
      i.versionId = v.id;
      i.sizeBytes = fi.size();
      issues.append(i);
    }
  }
  return issues;
}

} // namespace paleo::health
