// 层：数据
// 外链源重定位（relocateVersionSource）。原外部 office 转 PDF/XLSX 管线已移除。
#include "dataimportservice.h"

#include "shacache.h"

#include "../catalog/datacatalog.h"

#include <QFile>
#include <QFileInfo>
#include <QThread>

#include "dataimport_internal.h"

using paleo::dataimport_detail::setError;

// ---------------------------------------------------------------------------
// wave4/runtime-resilience：外链源重定位（TODOS P3「重新定位文件」恢复路径）。
// 「找不到源文件」死胡同的出口——但出口不是换内容：流式重算候选文件 SHA-256，
// 与该版本入库时留底一致才接受。版本记录不可变：不改写旧记录，而是追加一条
// 同内容、指向新路径的外链 RAW 版本（extra.relocatedFrom 留血统），
// currentVersion（取最高 versionNumber）从此解析到新路径；dedup 的
// versionBySha256 只认文件仍在且重哈希一致的版本——死路径旧记录自动出局，
// 重导/预览两条链路都不需要特判。不一致 → 拒解，catalog 一字不动（不静默
// 换源）。新路径落在工程目录内也仍按 external 记（不升级为 managed——工程
// 目录内容物是 catalog 自己的产物，混入外部文件会破坏受管面语义）。
// ---------------------------------------------------------------------------
QString DataImportService::relocateVersionSource(const QString &versionId,
                                                 const QString &newPath, QString *error)
{
  QString internalError;
  if (!error)
    error = &internalError;
  else
    error->clear();
  const auto fail = [&](const QString &msg) -> QString {
    setError(error, msg);
    return QString();
  };

  // 审计 02 M-8：活 catalog 只在 owner 线程读写——跨线程调用如实拒绝。
  if (QThread::currentThread() != m_catalog->thread())
    return fail(offThreadError());
  if (m_projectDir.isEmpty())
    return fail(QStringLiteral("project dir is not set"));
  if (!m_catalogReady)
    return fail(QStringLiteral("catalog 拒绝写入：%1")
                    .arg(m_catalogOpenError.isEmpty()
                             ? QStringLiteral("catalog 打开失败")
                             : m_catalogOpenError));
  const CatalogVersion v = m_catalog->versionById(versionId);
  if (v.id.isEmpty())
    return fail(QStringLiteral("版本不存在: %1").arg(versionId));
  if (v.managed)
    return fail(QStringLiteral("受管版本不支持重定位（受管文件属于工程目录，丢失应重导）: %1")
                    .arg(v.path));
  if (v.sha256.isEmpty())
    return fail(QStringLiteral("该版本入库时未留 SHA-256，无法核验新文件内容"));
  if (v.sha256.startsWith(QLatin1String("quick:")))
    return fail(QStringLiteral("快速指纹来源请通过文档会话的异步来源命令重定位"));
  if (newPath.isEmpty() || !QFile::exists(newPath))
    return fail(QStringLiteral("找不到源文件: %1").arg(newPath));
  const QFileInfo fi(newPath);
  if (!fi.isFile())
    return fail(QStringLiteral("不是普通文件: %1").arg(newPath));
  if (!DataCatalog::isSafePathSegment(fi.fileName()))
    return fail(QStringLiteral("文件名不是合法路径段: %1").arg(fi.fileName()));
  const QString abs = fi.absoluteFilePath();
  if (abs == v.path)
    return versionId; // 幂等：同一文件已在原位恢复，不动 catalog

  // 流式 SHA-256 复验（与导入/外链校验同一面）。
  QString herr;
  const QString sha = ShaCache::shared().sha256Hex(abs, &herr); // D7.7
  if (sha.isEmpty())
    return fail(herr.isEmpty() ? QStringLiteral("cannot hash %1").arg(abs) : herr);
  if (sha.compare(v.sha256, Qt::CaseInsensitive) != 0)
    return fail(QStringLiteral("文件内容与原版本不符（SHA-256 不一致）——已拒绝重定位"));

  CatalogVersion relocated;
  relocated.id = m_catalog->nextVersionId();
  relocated.assetId = v.assetId;
  relocated.stage = v.stage.isEmpty() ? QStringLiteral("RAW") : v.stage;
  relocated.versionNumber =
      m_catalog->currentVersion(v.assetId).versionNumber + 1;
  relocated.managed = false;
  relocated.path = abs;
  relocated.sourceUri = abs;
  relocated.sha256 = v.sha256; // 已验证一致——沿用留底值
  relocated.fileName = fi.fileName();
  relocated.extra = v.extra;   // 外链夹带的图例等元数据随内容一起搬
  relocated.extra.remove(QStringLiteral("nativeModel")); // Rebinding a path cannot reuse a source record pointing at the old file.
  relocated.extra.insert(QStringLiteral("relocatedFrom"), v.id);
  if (!m_catalog->addVersion(relocated, error))
    return fail(error->isEmpty() ? QStringLiteral("catalog addVersion failed") : *error);
  qInfo("relocate: %s -> %s (asset %s, from version %s)", qPrintable(v.path),
        qPrintable(abs), qPrintable(v.assetId), qPrintable(v.id));
  return relocated.id;
}
