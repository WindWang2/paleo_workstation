// 层：功能
#include "welltopseditorworkflow.h"
#include "workflowerrors_internal.h"

#include "../catalog/datacatalog.h"
#include "../domain/arearules.h"
#include "../io/wellfileparsers.h"
#include "derivedassets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace
{
using paleo::workflow_detail::setError;

bool readAllBytes(const QString &path, QByteArray *out, QString *error)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("无法读取版本文件：%1（%2）").arg(path, f.errorString()));
    return false;
  }
  *out = f.readAll(); // 空文件合法：解析结果为空行集
  return true;
}

bool writeAtomic(const QString &path, const QByteArray &bytes, QString *error)
{
  const QString tmp = path + QStringLiteral(".partial");
  {
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
      setError(error, QStringLiteral("无法写入暂存文件：%1（%2）").arg(tmp, f.errorString()));
      return false;
    }
    if (f.write(bytes) != bytes.size())
    {
      f.remove(); // 不留半截暂存件
      setError(error, QStringLiteral("暂存文件写入不完整：%1").arg(tmp));
      return false;
    }
  }
  if (QFile::exists(path))
    QFile::remove(path);
  if (!QFile::rename(tmp, path))
  {
    QFile::remove(tmp); // 落位失败回收暂存件
    setError(error, QStringLiteral("无法把暂存文件落位：%1 ← %2").arg(path, tmp));
    return false;
  }
  return true;
}
} // namespace

WellTopsEditorWorkflow::WellTopsEditorWorkflow(DataCatalog *catalog, const QString &projectDir)
  : m_catalog(catalog)
  , m_projectDir(projectDir)
{
}

bool WellTopsEditorWorkflow::loadAllRows(const QString &assetId, QVector<WellTopRecord> *rows,
                                         QString *error) const
{
  if (!m_catalog || !m_catalog->isOpen())
  {
    setError(error, QStringLiteral("catalog 未打开——无法读取分层"));
    return false;
  }
  const CatalogVersion v = m_catalog->currentVersion(assetId);
  if (v.id.isEmpty())
  {
    setError(error, QStringLiteral("资产无可用版本：%1").arg(assetId));
    return false;
  }
  const QString path = DataCatalog::resolvedVersionPath(m_projectDir, v);
  if (path.isEmpty() || !QFile::exists(path))
  {
    setError(error, QStringLiteral("版本文件缺失：%1").arg(path));
    return false;
  }
  QByteArray bytes;
  QString readErr;
  if (!readAllBytes(path, &bytes, &readErr))
  {
    setError(error, readErr);
    return false;
  }
  *rows = parseWellTopsText(bytes);
  return true;
}

QVector<WellTopRecord> WellTopsEditorWorkflow::rowsForWell(const QVector<WellTopRecord> &all,
                                                           const QString &wellName)
{
  const QString normalized = DataCatalog::normalizeWellName(wellName);
  QVector<WellTopRecord> out;
  for (const WellTopRecord &r : all)
    if (DataCatalog::normalizeWellName(r.wellName) == normalized)
      out.append(r);
  return out;
}

QStringList WellTopsEditorWorkflow::wellNamesIn(const QVector<WellTopRecord> &all)
{
  QStringList out;
  QSet<QString> seen;
  for (const WellTopRecord &r : all)
  {
    const QString display = r.wellName.trimmed();
    const QString key = DataCatalog::normalizeWellName(r.wellName);
    if (!seen.contains(key))
    {
      seen.insert(key);
      out.append(display.isEmpty() ? key : display);
    }
  }
  return out;
}

WellTopsEdit::ValidationContext WellTopsEditorWorkflow::contextFor(const DataCatalog *catalog,
                                                                   const QString &wellName)
{
  WellTopsEdit::ValidationContext ctx;
  ctx.framework = AreaRules::active().sequenceBoundaries;
  if (!catalog || wellName.isEmpty())
    return ctx;
  const QStringList ids = catalog->wellsMatchingName(wellName);
  if (ids.size() != 1)
    return ctx; // 0 或 2+ 候选：TD 不可靠，如实不用（不猜）
  const CatalogEntity e = catalog->entityById(ids.front());
  if (e.id.isEmpty() || e.td <= 0.0)
    return ctx;
  ctx.hasTd = true;
  ctx.td = e.td;
  return ctx;
}

WellTopsEditorWorkflow::CommitOutcome WellTopsEditorWorkflow::commitWellRows(
    const QString &assetId, const QString &wellName, const QVector<WellTopRecord> &newRows,
    const QString &note, const QString &editKind)
{
  QVector<WellTopRecord> all;
  QString err;
  if (!loadAllRows(assetId, &all, &err))
    return {.ok = false, .error = err};

  const QString normalized = DataCatalog::normalizeWellName(wellName);
  QVector<WellTopRecord> merged;
  merged.reserve(all.size());
  bool replaced = false;
  for (const WellTopRecord &r : all)
  {
    const bool isWell = DataCatalog::normalizeWellName(r.wellName) == normalized;
    if (!isWell)
    {
      merged.append(r);
      continue;
    }
    if (!replaced)
    {
      // 该井行块首行锚定：编辑行整块替换进原位，其余井的行序保持文件序。
      merged += newRows;
      replaced = true;
    }
  }
  if (!replaced && !newRows.isEmpty())
    merged += newRows; // 原文件无此井（新井块追加）

  const WellTopsEdit::DiffSummary wellDiff = WellTopsEdit::diff(rowsForWell(all, wellName), newRows);
  return commitBytes(assetId, writeWellTopsText(merged),
                     editKind.isEmpty() ? QStringLiteral("edit") : editKind, wellName, wellDiff,
                     note, QString());
}

WellTopsEditorWorkflow::CommitOutcome WellTopsEditorWorkflow::commitAllRows(
    const QString &assetId, const QVector<WellTopRecord> &newAllRows, const QString &editKind,
    const QString &note)
{
  QVector<WellTopRecord> all;
  QString err;
  if (!loadAllRows(assetId, &all, &err))
    return {.ok = false, .error = err};
  return commitBytes(assetId, writeWellTopsText(newAllRows), editKind, QString(),
                     WellTopsEdit::diff(all, newAllRows), note, QString());
}

WellTopsEditorWorkflow::CommitOutcome WellTopsEditorWorkflow::rollbackTo(const QString &assetId,
                                                                         const QString &targetVersionId)
{
  if (!m_catalog)
    return {.ok = false, .error = QStringLiteral("workflow 未绑定 catalog")};
  const CatalogVersion target = m_catalog->versionById(targetVersionId);
  if (target.id.isEmpty() || target.assetId != assetId)
    return {.ok = false,
            .error = QStringLiteral("回滚目标不是该资产的版本：%1").arg(targetVersionId)};
  const CatalogVersion current = m_catalog->currentVersion(assetId);
  if (target.id == current.id)
    return {.ok = false, .error = QStringLiteral("目标版本已是当前版本")};
  const QString path = DataCatalog::resolvedVersionPath(m_projectDir, target);
  if (path.isEmpty() || !QFile::exists(path))
    return {.ok = false, .error = QStringLiteral("回滚目标文件缺失：%1").arg(path)};
  QByteArray bytes;
  QString readErr;
  if (!readAllBytes(path, &bytes, &readErr))
    return {.ok = false, .error = readErr};

  QVector<WellTopRecord> all;
  QString err;
  if (!loadAllRows(assetId, &all, &err))
    return {.ok = false, .error = err};
  return commitBytes(assetId, bytes, QStringLiteral("rollback"), QString(),
                     WellTopsEdit::diff(all, parseWellTopsText(bytes)), QString(),
                     targetVersionId);
}

bool WellTopsEditorWorkflow::loadMergeRows(const QString &assetId, const QString &wellName,
                                           const QString &externalPath,
                                           QVector<WellTopsEdit::MergeRow> *rows,
                                           QString *error) const
{
  QVector<WellTopRecord> all;
  QString err;
  if (!loadAllRows(assetId, &all, &err))
  {
    setError(error, err);
    return false;
  }
  QByteArray bytes;
  QString readErr;
  if (!readAllBytes(externalPath, &bytes, &readErr))
  {
    setError(error, readErr);
    return false;
  }
  const QVector<WellTopRecord> incoming = rowsForWell(parseWellTopsText(bytes), wellName);
  if (incoming.isEmpty())
  {
    setError(error, QStringLiteral("文件中没有井「%1」的分层行").arg(wellName));
    return false;
  }
  *rows = WellTopsEdit::mergeDiff(rowsForWell(all, wellName), incoming);
  return true;
}

QStringList WellTopsEditorWorkflow::validateAllWells(const DataCatalog *catalog,
                                                     const QVector<WellTopRecord> &allRows)
{
  QStringList errors;
  // 井分组按 catalog 规范化键（与 rowsForWell/commitWellRows 同一口径）。
  QStringList order;
  QHash<QString, QString> keyToWell;
  QHash<QString, QVector<WellTopRecord>> byWell;
  for (const WellTopRecord &r : allRows)
  {
    const QString key = DataCatalog::normalizeWellName(r.wellName);
    if (!keyToWell.contains(key))
    {
      keyToWell.insert(key, r.wellName);
      order.append(key);
    }
    byWell[key].append(r);
  }
  for (const QString &key : order)
  {
    const QString well = keyToWell.value(key);
    const WellTopsEdit::ValidationContext ctx = contextFor(catalog, well);
    for (const WellTopsEdit::Issue &issue : WellTopsEdit::validate(byWell.value(key), ctx))
      if (issue.isError())
        errors.append(QStringLiteral("[%1] %2").arg(well, issue.message));
  }
  return errors;
}

QVector<WellTopsEditorWorkflow::VersionInfo> WellTopsEditorWorkflow::versionHistory(
    const QString &assetId) const
{
  QVector<VersionInfo> out;
  if (!m_catalog)
    return out;
  for (const CatalogVersion &v : m_catalog->versionsForAsset(assetId))
  {
    VersionInfo info;
    info.id = v.id;
    info.versionNumber = v.versionNumber;
    info.stage = v.stage;
    info.editKind = v.extra.value(QStringLiteral("editKind")).toString();
    info.editWell = v.extra.value(QStringLiteral("editWell")).toString();
    info.stale = v.extra.value(QStringLiteral("stale")).toBool();
    out.append(info);
  }
  std::sort(out.begin(), out.end(),
            [](const VersionInfo &a, const VersionInfo &b)
            { return a.versionNumber > b.versionNumber; });
  return out;
}

WellTopsEditorWorkflow::CommitOutcome WellTopsEditorWorkflow::commitBytes(
    const QString &assetId, const QByteArray &bytes, const QString &editKind,
    const QString &wellName, const WellTopsEdit::DiffSummary &diff, const QString &note,
    const QString &rollbackTarget)
{
  if (!m_catalog || !m_catalog->isOpen())
    return {.ok = false, .error = QStringLiteral("catalog 未打开——无法提交分层编辑")};
  const CatalogAsset asset = m_catalog->assetById(assetId);
  if (asset.id.isEmpty())
    return {.ok = false, .error = QStringLiteral("资产不存在：%1").arg(assetId)};
  const CatalogVersion current = m_catalog->currentVersion(assetId);
  if (current.id.isEmpty())
    return {.ok = false, .error = QStringLiteral("资产无基线版本，编辑无从落库")};

  const QString fileName = current.fileName.isEmpty() ? asset.displayName : current.fileName;
  DerivedAssetRegistrar registrar(m_catalog, m_projectDir);
  QString stageErr;
  const DerivedStaging st = registrar.stage(asset.type, asset.displayName, fileName, &stageErr);
  if (!st.isValid())
    return {.ok = false, .error = stageErr};
  if (st.assetId != assetId)
    return {.ok = false,
            .error = QStringLiteral("同名资产多份，版本会落错资产（%1 ≠ %2）——请先在数据页"
                                    "区分资产名再编辑")
                        .arg(st.assetId, assetId)};
  if (!writeAtomic(st.absolutePath, bytes, &stageErr))
    return {.ok = false, .error = stageErr};

  // 与当前版本字节一致 = 无变化：如实不发版本（不空涨 revision）。
  {
    QString shaErr;
    const QString newSha = DataCatalog::sha256FileHex(st.absolutePath, &shaErr);
    QString currentSha = current.sha256;
    if (currentSha.isEmpty() && current.managed)
      currentSha = DataCatalog::sha256FileHex(
          DataCatalog::resolvedVersionPath(m_projectDir, current), &shaErr);
    if (!newSha.isEmpty() && !currentSha.isEmpty() && newSha == currentSha)
    {
      QFile::remove(st.absolutePath); // 回收暂存件——无版本孤儿文件不留
      CommitOutcome out;
      out.ok = true;
      out.unchanged = true;
      out.diff = diff;
      return out;
    }
  }

  QVariantMap extra;
  extra.insert(QStringLiteral("editKind"), editKind);
  if (!wellName.isEmpty())
    extra.insert(QStringLiteral("editWell"), wellName);
  extra.insert(QStringLiteral("rowsAdded"), diff.added);
  extra.insert(QStringLiteral("rowsRemoved"), diff.removed);
  extra.insert(QStringLiteral("rowsChanged"), diff.changed);
  extra.insert(QStringLiteral("editBaseline"), current.id);
  extra.insert(QStringLiteral("editBaselineNumber"), current.versionNumber);
  if (!note.isEmpty())
    extra.insert(QStringLiteral("editNote"), note);
  if (!rollbackTarget.isEmpty())
    extra.insert(QStringLiteral("rollbackTo"), rollbackTarget);

  CommitOutcome out;
  out.diff = diff;
  // parentVersionIds 刻意留空：见头注释「血缘口径」——supersede 语义会把
  // 旧版本闭包（含以旧版本为父的本新版本）标 stale。
  QString commitErr;
  if (!registrar.commit(st, {}, QStringLiteral("paleo:welltops/%1").arg(editKind), extra,
                        &commitErr))
  {
    QFile::remove(st.absolutePath); // 提交失败回收暂存件，不留无版本孤儿文件
    return {.ok = false, .error = commitErr};
  }

  out.ok = true;
  out.newVersionId = st.versionId;
  out.newVersionNumber = st.versionNumber;
  return out;
}
