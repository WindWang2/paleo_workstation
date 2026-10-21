// 层：数据
#include "dataimport_log.h"
#include "dataimport_internal.h"
#include "welllogread.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <cmath>
#include <limits>

namespace paleo::dataimport_detail
{
namespace
{
QString tr(const char *s) { return QCoreApplication::translate("WellLogImport", s); }
bool reject(QString *error, const QString &message)
{
  if (error) *error = message;
  return false;
}
QString ensureWell(DataCatalog &cat, const QString &name, QString *error)
{
  if (name.trimmed().isEmpty())
  {
    reject(error, tr("请输入新井名称。"));
    return {};
  }
  // F6（review-security）：规范化后为空的名字（如 "-"、"___"）会让
  // wellsMatchingName 恒零匹配、井名互撞检测失效——拒绝建井。
  if (DataCatalog::normalizeWellName(name).isEmpty())
  {
    reject(error, tr("井名 %1 不是有效名称（缺少字母或数字），请检查文件的井名字段。").arg(name));
    return {};
  }
  const auto matches = cat.wellsMatchingName(name);
  if (matches.size() == 1 && cat.entityById(matches.first()).name.trimmed().compare(
          name.trimmed(), Qt::CaseInsensitive) == 0) return matches.first();
  if (!matches.isEmpty())
  {
    if (matches.size() == 1)
      reject(error, tr("井名 %1 与已有井 %2 规范化后相同（原始名不同），请显式选择该井后再导入。没有创建或合并数据。")
                 .arg(name, cat.entityById(matches.first()).name));
    else
      reject(error, tr("井名 %1 对应多口井，请选择已有井。没有创建或合并数据。").arg(name));
    return {};
  }
  CatalogEntity well;
  well.id = cat.nextEntityId(QStringLiteral("well"));
  well.entityType = QStringLiteral("well");
  well.name = name.trimmed();
  // 没有坐标证据的井可管理曲线，但不伪造地图位置。
  well.coordinateStatus = QStringLiteral("unknown");
  return cat.addEntity(well, error) ? well.id : QString();
}
}

bool inspectLog(const QString &path, LogInspection &out, QString *error)
{
  out = {};
  if (error) error->clear();
  QList<LasIssue> issues;
  LasDoc doc;
  if (!WellLogRead::parse(path, out.header, doc.curves, error, &issues)) return false;
  doc.curveNames = out.header.curveNames;
  if (doc.curves.size() < 2 || doc.curves.first().values.isEmpty())
    return reject(error, tr("%1：没有可用的索引和测井曲线，请检查数据表头。").arg(path));
  const auto &axis = doc.curves.first();
  for (int c = 1; c < doc.curves.size(); ++c)
    if (doc.curves[c].values.size() != axis.values.size())
      return reject(error, tr("%1：曲线 %2 与索引样本数不同，暂未导入。").arg(path, doc.curves[c].name));
  int direction = 0;
  for (int i = 0; i < axis.values.size(); ++i)
  {
    if (!std::isfinite(axis.values[i]))
      return reject(error, tr("%1：第 %2 个索引不是有限数值，请检查源文件。").arg(path).arg(i + 1));
    if (!i) continue;
    const double delta = axis.values[i] - axis.values[i - 1];
    const int current = delta > 0 ? 1 : delta < 0 ? -1 : 0;
    if (!current || (direction && direction != current))
      return reject(error, tr("%1：第 %2 个索引重复或顺序异常，请检查源文件。").arg(path).arg(i + 1));
    direction = current;
  }
  QStringList labels;
  for (int c = 1; c < doc.curves.size(); ++c)
    labels << (doc.curves[c].unit.isEmpty() ? doc.curves[c].name
        : QStringLiteral("%1 [%2]").arg(doc.curves[c].name, doc.curves[c].unit));
  const QString unit = axis.unit.isEmpty() ? tr("单位未声明") : axis.unit;
  out.summary = tr("%1 条曲线：%2；%3 %4–%5 %6；%7 个索引点")
      .arg(doc.curves.size() - 1).arg(labels.join(QStringLiteral(", ")), axis.name)
      .arg(axis.values.first(), 0, 'g', 12).arg(axis.values.last(), 0, 'g', 12)
      .arg(unit).arg(axis.values.size());
  QStringList table{doc.curveNames.mid(0, 5).join(QLatin1Char('\t'))};
  for (int i = 0; i < qMin<qsizetype>(5, axis.values.size()); ++i)
  {
    QStringList cells;
    for (int c = 0; c < qMin<qsizetype>(5, doc.curves.size()); ++c)
      cells << (std::isfinite(doc.curves[c].values[i])
          ? QString::number(doc.curves[c].values[i], 'g', 10) : tr("缺失"));
    table << cells.join(QLatin1Char('\t'));
  }
  out.preview = table.join(QLatin1Char('\n'));
  for (const auto &issue : issues)
  {
    if (issue.severity == LasIssue::Severity::Error)
      return reject(error, tr("%1：%2").arg(path, issue.message));
    if (!out.issues.contains(issue.message)) out.issues << issue.message;
  }
  if (axis.unit.isEmpty()) out.issues << tr("索引单位未声明，保留原始数值；需要换算的业务将要求补充单位。");
  if (axis.unit.compare(QLatin1String("ft"), Qt::CaseInsensitive) == 0)
    out.issues << tr("索引单位为英尺，保留原始文件和单位；需要米的业务会明确换算。");
  return true;
}

void fillLogPreview(const DataCatalog &cat, FolderPreviewRow &row)
{
  for (const auto &well : cat.entities(QStringLiteral("well")))
    row.availableWells.insert(well.id, well.name);
  if (row.classifiedType != QLatin1String("well_log") || row.skipped) return;
  LogInspection info;
  QString error;
  const QString before = logFileDigest(row.path);
  if (!inspectLog(row.path, info, &error))
  {
    row.importBlocked = true;
    row.issues << error;
    row.summary = tr("此文件暂不能导入，请检查错误后重试；其他有效文件可继续。");
    return;
  }
  row.summary = info.summary;
  row.sourceSha256 = logFileDigest(row.path);
  if (before.isEmpty() || row.sourceSha256 != before)
  {
    row.importBlocked = true;
    row.issues << tr("预览期间文件内容发生变化，请重新选择文件生成预览。");
    return;
  }
  row.dataPreview = info.preview;
  row.issues = info.issues;
  row.proposedWellName = info.header.wellName.trimmed();
  const auto matches = cat.wellsMatchingName(row.proposedWellName);
  if (matches.size() == 1 && cat.entityById(matches.first()).name.trimmed().compare(
          row.proposedWellName, Qt::CaseInsensitive) == 0)
  {
    row.suggestedWellId = matches.first();
    row.entityPreview = tr("井 %1").arg(cat.entityById(matches.first()).name);
    // 已有资产的曲线目录来自导入时元数据；旧版本缺少该字段不误判冲突。
    QSet<QString> names;
    for (const auto &link : cat.linksForEntity(matches.first()))
      if (link.role == QLatin1String("well_log") && !link.unresolved)
        for (const auto &version : cat.versionsForAsset(link.assetId))
          for (const auto &name : version.extra.value(QStringLiteral("logCurves")).toStringList()) names.insert(name);
    QStringList collisions;
    for (const auto &name : info.header.curveNames.mid(1))
      if (names.contains(name)) collisions << name;
    if (!collisions.isEmpty() && row.decision != QLatin1String("skip"))
      row.issues << tr("与已有曲线同名：%1。确认后保留两份并标明来源，不覆盖已有数据。").arg(collisions.join(QStringLiteral(", ")));
  }
  else if (!row.proposedWellName.isEmpty() && matches.isEmpty())
    row.entityPreview = tr("新井 %1（来自文件内容）").arg(row.proposedWellName);
  else
  {
    row.needsWellChoice = row.decision != QLatin1String("skip");
    row.entityPreview = tr("请选择目标井");
    row.issues << (!matches.isEmpty()
        ? tr("已有井名相似或对应多口井，请选择目标井；系统不会自动合并。")
        : tr("已识别测井曲线，但无法确定所属井。文件名仅作线索，请选择目标井或新建井。"));
  }
}

QString importInspectedLog(FamilyContext &ctx)
{
  QString target = ctx.targetWellId;
  QString error;
  if (target.startsWith(QLatin1String("new:")))
  {
    const QString name = target.mid(4).trimmed();
    // F4（review-security）：「新建井」撞名不静默并入——已有精确同名井时如实
    // 拒绝，由用户改选已有井或改名；并入只能发生在用户知情处（预览/确认表）。
    for (const auto &well : ctx.cat->entities(QStringLiteral("well")))
      if (well.name.trimmed().compare(name, Qt::CaseInsensitive) == 0)
        return tr("井 %1 已存在：未新建井，本文件暂未导入。如需归入该井，请重新预览并选择「已有井」。")
            .arg(name);
    target = ensureWell(*ctx.cat, name, &error);
  }
  else if (!target.isEmpty())
  {
    const auto well = ctx.cat->entityById(target);
    if (well.entityType != QLatin1String("well")) return tr("所选井已不存在，请刷新预览后重试。");
  }
  else
    target = ensureWell(*ctx.cat, ctx.logWellName, &error);
  if (target.isEmpty()) return error.isEmpty() ? tr("无法确定所属井，请在导入预览中选择目标井。") : error;
  EntityAssetLink link;
  link.assetId = ctx.assetId;
  link.entityType = QStringLiteral("well");
  link.entityId = target;
  link.role = QStringLiteral("well_log");
  int maxOrdinal = -1;
  bool primary = false;
  for (const auto &old : ctx.cat->linksForEntity(target))
    if (old.role == link.role && !old.unresolved)
    {
      maxOrdinal = qMax(maxOrdinal, old.ordinal);
      primary |= old.isPrimary;
    }
  link.isPrimary = !primary;
  if (maxOrdinal == std::numeric_limits<int>::max()) return tr("井下数据数量超过排序上限，暂未导入。");
  link.ordinal = maxOrdinal + 1;
  if (!ctx.targetWellId.isEmpty())
    link.note = tr("用户在导入预览中确认归属；文件井名：%1").arg(ctx.logWellName);
  return ctx.cat->addLink(link, &error) ? QString() : error;
}

QString logFileDigest(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

bool validateDuplicateLogTarget(DataCatalog &cat, const QString &assetId,
                               const QString &choice, QString *error)
{
  if (choice.isEmpty()) return true;
  QString target = choice;
  if (choice.startsWith(QLatin1String("new:")))
  {
    const QString name = choice.mid(4).trimmed();
    target.clear();
    for (const auto &well : cat.entities(QStringLiteral("well")))
      if (well.name.trimmed().compare(name, Qt::CaseInsensitive) == 0)
      {
        if (!target.isEmpty()) return reject(error, tr("井名存在歧义，请选择已有井。"));
        target = well.id;
      }
  }
  if (!target.isEmpty())
    for (const auto &link : cat.linksForAsset(assetId))
      if (link.role == QLatin1String("well_log") && !link.unresolved && link.entityId == target)
        return true;
  return reject(error, tr("这份数据已经存在，所属井与本次选择不同。未更改归属，也未新建井；请在数据属性中核对关联。"));
}
}
