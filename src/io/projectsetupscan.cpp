// 层：数据
#include "projectsetupscan.h"
#include "importclassification.h"
#include "welltable.h"
#include "wellfileparsers.h"
#include "welllogread.h"
#include "../metadata/nativereference.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace {
bool excluded(const QString &name)
{
  return name.startsWith('.') || name.startsWith("~$") ||
      QStringList{"_旧项目工作区", "artifacts", "basemap", "backup", "backups", "备份", "旧项目", "缓存"}.contains(name, Qt::CaseInsensitive);
}
QStringList collect(const QString &path, QStringList &notes, const std::function<bool()> &cancelled)
{
  if (path.isEmpty()) return {};
  const QFileInfo info(path);
  if (info.isFile()) return {info.absoluteFilePath()};
  QStringList result, queue{path};
  while (!queue.isEmpty()) {
    if (cancelled && cancelled()) return {};
    const QDir directory(queue.takeFirst());
    for (const auto &f : directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      if (f.isSymLink() || excluded(f.fileName())) {
        notes << QObject::tr("已排除旧工程、缓存或链接：%1").arg(f.absoluteFilePath()); continue;
      }
      if (f.isDir()) queue << f.absoluteFilePath();
      else if (!f.size()) notes << QObject::tr("空文件已跳过：%1").arg(f.fileName());
      else if (!QStringList{"qgz", "qgs", "paleo", "qml", "psx", "lock", "sqlite", "db"}.contains(f.suffix().toLower())) result << f.absoluteFilePath();
    }
  }
  return result;
}
QString topKey(const WellTopRecord &r)
{
  return r.wellName.trimmed().toUpper() + '|' + r.topName.trimmed().toUpper() + '|' +
      "MD:" + (r.hasMd ? QString::number(r.md, 'g', 17) : QStringLiteral("missing")) +
      "|TVD:" + (r.hasTvd ? QString::number(r.tvd, 'g', 17) : QStringLiteral("missing"));
}
void appendFile(ProjectSetupPreview &out, const QString &path, const QString &type)
{
  out.paths << path; out.types.insert(path, type);
  if (QFileInfo(path).size() <= 200ll * 1024 * 1024) {
    QFile f(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (f.open(QIODevice::ReadOnly) && hash.addData(&f)) out.hashes.insert(path, QString::fromLatin1(hash.result().toHex()));
    else out.errors << QObject::tr("无法核对源文件：%1").arg(path);
  }
}
}

ProjectSetupSources discoverProjectSources(const QString &root)
{
  ProjectSetupSources s; s.root = root;
  QDir d(root);
  for (const auto &file : d.entryInfoList(QDir::Files, QDir::Name)) {
    const QString name = file.completeBaseName();
    if (s.points.isEmpty() && (name.contains(QStringLiteral("井坐标")) || name.contains(QStringLiteral("井位")) || name.contains("wellhead", Qt::CaseInsensitive))) s.points = file.absoluteFilePath();
  }
  const auto find = [&](const QStringList &names) {
    for (const auto &name : names) if (QFileInfo(d.filePath(name)).isDir()) return d.filePath(name);
    return QString();
  };
  s.wells = find({QStringLiteral("井"), QStringLiteral("井数据"), "wells"});
  s.seismic = find({QStringLiteral("地震"), "seismic"});
  s.horizons = find({QStringLiteral("层位"), "horizons"});
  s.references = find({QStringLiteral("参考资料"), "references"});
  s.paleomaps = find({QStringLiteral("古地理图"), QStringLiteral("现有古地理图"), "paleomaps", "historical_facies_map"});
  return s;
}

ProjectSetupPreview scanProjectSources(const ProjectSetupSources &s, const std::function<bool()> &cancelled)
{
  ProjectSetupPreview out;
  scanProjectMaps(s, &out, cancelled);
  QSet<QString> seen;
  struct Candidate { QString path, type; paleo::io::WellTable table; };
  QVector<Candidate> tables;
  QSet<QString> wellNames;
  QStringList logPaths;
  const QStringList sources{s.points, s.wells, s.seismic, s.horizons, s.references, s.boundary};
  for (const auto &path : sources) {
    if (path.isEmpty()) continue;
    if (!QFileInfo::exists(path)) { out.errors << QObject::tr("资料路径不存在：%1").arg(path); continue; }
    for (const auto &file : collect(path, out.notes, cancelled)) {
      if (cancelled && cancelled()) { out.errors << QObject::tr("已取消扫描"); return out; }
      const QString key = QFileInfo(file).canonicalFilePath();
      if (QFileInfo(s.paleomaps).isDir() && key.startsWith(QFileInfo(s.paleomaps).canonicalFilePath() + '/')) continue;
      if (seen.contains(key)) continue;
      seen.insert(key);
      if (!QFileInfo(file).size()) { out.notes << QObject::tr("空文件已跳过：%1").arg(file); continue; }
      const bool points = QFileInfo(file) == QFileInfo(s.points), boundary = QFileInfo(file) == QFileInfo(s.boundary);
      auto cls = classifyImportFile(file);
      if (boundary) cls.type = QStringLiteral("boundary");
      if (boundary && out.sourceCrs.isEmpty()) out.sourceCrs = paleo::io::readWellTable(file).crs;
      if (points || cls.type == QLatin1String("well_stratification")) {
        if (paleo::io::isWellTableContainer(file) || points) {
          auto table = paleo::io::readWellTable(file, points ? s.pointMapping : QVariantMap{});
          if (points) {
            out.sheets = table.sheets; out.columns = table.columns; out.pointRows = table.preview;
            out.pointMapping = table.mapping; out.sourceCrs = table.crs;
            if (!table.error.isEmpty() || table.heads.isEmpty()) { out.errors << QObject::tr("井点：%1").arg(table.error); continue; }
            cls.type = QStringLiteral("well_head"); out.mappings.insert(file, table.mapping);
            for (const auto &h : table.heads) wellNames.insert(h.name);
            out.notes << table.report.issues;
          } else { tables << Candidate{file, cls.type, table}; continue; }
        } else if (points) {
          QFile f(file); if (f.open(QIODevice::ReadOnly)) for (const auto &h : parseWellHeadText(f.readAll())) wellNames.insert(h.name);
          cls.type = QStringLiteral("well_head"); out.sourceCrs = s.pointMapping.value("crs").toString();
        }
      }
      if (cls.type == QLatin1String("well_log")) { ++out.logs; logPaths << file; }
      else if (cls.type == QLatin1String("seismic")) ++out.seismic;
      else if (cls.type == QLatin1String("horizon")) {
        ++out.horizons;
        QFile input(file);
        if (input.open(QIODevice::ReadOnly)) {
          const QString crs = paleo::io::wellSourceCrsHint(QString::fromUtf8(input.read(8192)));
          if (out.sourceCrs.isEmpty()) out.sourceCrs = crs;
          else if (!crs.isEmpty() && !sameNativeCrs(out.sourceCrs, crs)) out.errors << QObject::tr("层位坐标系与井点不一致，请先统一坐标或暂不导入：%1").arg(file);
        }
      }
      else if (cls.type != QLatin1String("well_head") && cls.type != QLatin1String("boundary") && cls.type != QLatin1String("time_depth")) ++out.references;
      appendFile(out, file, cls.type);
    }
  }
  // Largest aggregate table first; skip only exact record subsets, not files
  // with a coincident name. Conflicting depths survive for explicit review.
  std::stable_sort(tables.begin(), tables.end(), [](const auto &a, const auto &b) { return a.table.tops.size() > b.table.tops.size(); });
  QSet<QString> topRows;
  for (const auto &t : tables) {
    QSet<QString> keys;
    for (const auto &r : t.table.tops) keys.insert(topKey(r));
    if (!keys.isEmpty() && (keys - topRows).isEmpty()) { out.notes << QObject::tr("分层内容重复，已跳过：%1").arg(QFileInfo(t.path).fileName()); continue; }
    out.tops += keys.size(); topRows.unite(keys);
    out.notes << t.table.report.issues;
    appendFile(out, t.path, t.type);
  }
  out.wells = wellNames.size();
  out.knownWells = wellNames.values(); out.knownWells.sort();
  for (const auto &path : logPaths) {
    QString content;
    WellLogRead::readWellInfo(path, content);
    if (wellNames.isEmpty() || wellNames.contains(content)) continue;
    out.logWellNames.insert(path, content);
    const QString parent = QFileInfo(path).dir().dirName();
    const QString proposed = wellNames.contains(parent) ? QStringLiteral("name:") + parent : QString();
    out.wellTargets.insert(path, s.wellTargets.contains(path) ? s.wellTargets.value(path) : proposed);
    out.notes << QObject::tr("井名冲突：%1 内部井名为 %2，请在上方归属表确认建议或保留内部井名。").arg(QFileInfo(path).fileName(), content);
  }
  if (!out.sourceCrs.isEmpty() && !validNativeCrs(out.sourceCrs)) out.errors << QObject::tr("无法识别资料坐标系：%1，请填写有效的 EPSG 编号。").arg(out.sourceCrs);
  if (out.sourceCrs.isEmpty() && s.points.isEmpty() && !out.seismic && !out.horizons)
    for (const auto &map : out.maps) if (map.included && !map.crs.isEmpty()) { out.sourceCrs = map.crs; break; }
  out.projectCrs = out.sourceCrs;
  if (!out.pointRows.isEmpty() && !out.sourceCrs.isEmpty()) {
    out.projectCrs = metricNativeCrs(out.sourceCrs, out.pointRows.first().value(1).toDouble(), out.pointRows.first().value(2).toDouble());
    if (!sameNativeCrs(out.sourceCrs, out.projectCrs)) {
      out.notes << QObject::tr("井点将转换到米制工程坐标 %1，原经纬度保留在源表。").arg(out.projectCrs);
      if (out.horizons || out.seismic) out.errors << QObject::tr("井点为经纬度且同时选择了地震或层位，请先确认它们的米制坐标基础后分别导入。");
    }
  }
  if (!s.points.isEmpty() && out.sourceCrs.isEmpty()) out.notes << QObject::tr("井点坐标系未确定：将按局部工程坐标保存；设置坐标系后可叠加网络底图。");
  if (out.logs > out.wells && out.wells) out.notes << QObject::tr("测井份数多于井数：同井多版资料分别保留，不自动混合曲线。");
  return out;
}
