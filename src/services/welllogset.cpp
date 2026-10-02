// 层：数据
#include "welllogset.h"

#include "catalog/datacatalog.h"
#include "io/lasparser.h"

#include <QFileInfo>
#include <QHash>

#include <algorithm>

// 已决 well_log 的只读并集。只走 LasParser::parseHeader（遇 ~A 即停），
// 不读数据体、不写 catalog。wellLogFiles 与 wellCurveIndex 各扫一次，
// 告警来自同一套判定。

namespace
{
  struct CurveItem
  {
    QString mnemonic;
    QString baseName;
    QString versionId;
    QString path;
    int column = -1;
    bool primary = false;
    bool aliased = false;
  };

  void resetWarnings(WellLogWarnings *warnings)
  {
    if (!warnings)
      return;
    warnings->count = 0;
    warnings->messages.clear();
  }

  void addWarning(WellLogWarnings *warnings, const QString &message)
  {
    if (!warnings)
      return;
    warnings->messages.append(message);
    warnings->count = warnings->messages.size();
  }

  QVector<CurveItem> itemsFromFiles(const QVector<WellLogFile> &files,
                                   WellLogWarnings *warnings)
  {
    QVector<CurveItem> items;
    for (const WellLogFile &file : files)
    {
      const QString base = QFileInfo(file.path).completeBaseName();
      QHash<QString, int> seen;
      for (int col = 1; col < file.curveNames.size(); ++col)
      {
        const QString raw = file.curveNames.at(col);
        const int n = seen.value(raw, 0);
        seen.insert(raw, n + 1);
        CurveItem item;
        item.baseName = base;
        item.versionId = file.versionId;
        item.path = file.path;
        item.column = col;
        item.primary = file.isPrimary;
        if (n == 0)
        {
          item.mnemonic = raw;
        }
        else
        {
          item.mnemonic = raw + QLatin1Char('#') + QString::number(col);
          addWarning(warnings,
                     QStringLiteral("%1: 同名曲线 %2 列 %3")
                         .arg(file.path, raw, QString::number(col)));
        }
        items.append(item);
      }
    }
    return items;
  }

  // 跨文件重名才加 @basename。无主文件（或主文件没有这一列）时重名列全部加，不挑主。
  // 别名仍撞车再追加 #versionId。
  void disambiguateAcrossFiles(QVector<CurveItem> &items)
  {
    QHash<QString, QVector<int>> groups;
    for (int i = 0; i < items.size(); ++i)
      groups[items.at(i).mnemonic].append(i);

    for (auto it = groups.cbegin(); it != groups.cend(); ++it)
    {
      const QString &name = it.key();
      const QVector<int> &idxs = it.value();
      bool multiFile = false;
      bool anyPrimary = false;
      const QString &firstPath = items.at(idxs.at(0)).path;
      for (int i : idxs)
      {
        if (items.at(i).path != firstPath)
          multiFile = true;
        if (items.at(i).primary)
          anyPrimary = true;
      }
      if (!multiFile)
        continue;
      for (int i : idxs)
      {
        if (anyPrimary && items.at(i).primary)
          continue;
        items[i].mnemonic = name + QLatin1Char('@') + items.at(i).baseName;
        items[i].aliased = true;
      }
    }

    QHash<QString, int> counts;
    for (const CurveItem &item : items)
      counts[item.mnemonic] = counts.value(item.mnemonic, 0) + 1;
    for (CurveItem &item : items)
    {
      if (item.aliased && counts.value(item.mnemonic) > 1)
        item.mnemonic += QLatin1Char('#') + item.versionId;
    }
  }

  struct Prepared
  {
    QVector<WellLogFile> files;
    QVector<CurveItem> items;
  };

  Prepared prepare(const DataCatalog *catalog, const QString &projectDir,
                   const QString &wellId, WellLogWarnings *warnings)
  {
    Prepared out;
    resetWarnings(warnings);
    // 未打开优先于空 wellId，调用方能区分「没打开」和「没有这口井」。
    if (!catalog || !catalog->isOpen())
    {
      addWarning(warnings, QStringLiteral("catalog 未打开"));
      return out;
    }
    if (wellId.isEmpty())
      return out;

    struct Row
    {
      WellLogFile file;
      int versionNumber = 0;
    };
    QVector<Row> rows;
    const QVector<EntityAssetLink> links = catalog->linksForEntity(wellId);
    for (const EntityAssetLink &link : links)
    {
      if (link.role != QLatin1String("well_log") || link.unresolved)
        continue;
      const CatalogVersion version = catalog->currentVersion(link.assetId);
      const QString path = DataCatalog::resolvedVersionPath(projectDir, version);
      const bool missing = version.id.isEmpty() || path.isEmpty() || !QFileInfo(path).isFile();
      if (missing)
      {
        const QString shown = !path.isEmpty() ? path
                              : !version.id.isEmpty() ? version.id
                                                      : link.assetId;
        addWarning(warnings, QStringLiteral("%1: 文件不存在").arg(shown));
        continue;
      }
      LasHeaderInfo header;
      QString error;
      if (!LasParser::parseHeader(path, header, &error))
      {
        addWarning(warnings, QStringLiteral("%1: %2").arg(path, error));
        continue;
      }
      Row row;
      row.file.assetId = link.assetId;
      row.file.versionId = version.id;
      row.file.path = path;
      row.file.isPrimary = link.isPrimary;
      row.file.ordinal = link.ordinal;
      row.file.curveNames = header.curveNames;
      row.versionNumber = version.versionNumber;
      rows.append(row);
    }

    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
      if (a.file.ordinal != b.file.ordinal)
        return a.file.ordinal < b.file.ordinal;
      if (a.versionNumber != b.versionNumber)
        return a.versionNumber < b.versionNumber;
      return a.file.versionId < b.file.versionId;
    });

    out.files.reserve(rows.size());
    for (const Row &row : rows)
      out.files.append(row.file);
    out.items = itemsFromFiles(out.files, warnings);
    return out;
  }

  QVector<WellCurveRef> refsFromItems(const QVector<CurveItem> &items)
  {
    QVector<WellCurveRef> refs;
    refs.reserve(items.size());
    for (const CurveItem &item : items)
    {
      WellCurveRef ref;
      ref.mnemonic = item.mnemonic;
      ref.sourceVersionId = item.versionId;
      ref.path = item.path;
      ref.column = item.column;
      ref.canonical = item.primary && !item.aliased;
      refs.append(ref);
    }
    return refs;
  }
} // namespace

QVector<WellLogFile> WellLogSet::wellLogFiles(const DataCatalog *catalog,
                                             const QString &projectDir,
                                             const QString &wellId,
                                             WellLogWarnings *warnings)
{
  return prepare(catalog, projectDir, wellId, warnings).files;
}

QVector<WellCurveRef> WellLogSet::wellCurveIndex(const DataCatalog *catalog,
                                                const QString &projectDir,
                                                const QString &wellId,
                                                WellLogWarnings *warnings)
{
  Prepared scanned = prepare(catalog, projectDir, wellId, warnings);
  disambiguateAcrossFiles(scanned.items);
  return refsFromItems(scanned.items);
}
