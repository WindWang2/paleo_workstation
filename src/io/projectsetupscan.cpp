// 层：数据
#include "projectsetupscan.h"
#include "importclassification.h"
#include "welltable.h"
#include "projectcoordinates.h"
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
  return name.startsWith('.') || name.startsWith("~$") || (name.startsWith("_") && (name.contains(QStringLiteral("备份")) || name.contains("backup", Qt::CaseInsensitive))) ||
      QStringList{"_旧项目工作区", "artifacts", "basemap", "backup", "backups", "备份", "旧项目", "缓存"}.contains(name, Qt::CaseInsensitive);
}
QStringList collect(const QString &path, ProjectSetupPreview &out, const std::function<bool()> &cancelled)
{
  if (path.isEmpty()) return {};
  const QFileInfo info(path);
  if (info.isFile()) return {info.absoluteFilePath()};
  QStringList result, queue{path};
  while (!queue.isEmpty()) {
    if (cancelled && cancelled()) return {};
    const QDir directory(queue.takeFirst());
    if (!QFileInfo(directory.absolutePath()).isReadable()) {
      out.sourceProblems.insert(directory.absolutePath(), QObject::tr("目录不可读，未扫描其中的资料")); continue;
    }
    for (const auto &f : directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
      if (f.isSymLink() || excluded(f.fileName())) {
        out.notes << QObject::tr("已排除旧工程、缓存或链接：%1").arg(f.absoluteFilePath()); continue;
      }
      if (f.isDir()) queue << f.absoluteFilePath();
      else if (!f.size()) out.notes << QObject::tr("空文件已跳过：%1").arg(f.fileName());
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
bool appendFile(ProjectSetupPreview &out, const QString &path, const QString &type)
{
  if (QFileInfo(path).size() <= 200ll * 1024 * 1024) {
    QFile f(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (f.open(QIODevice::ReadOnly) && hash.addData(&f)) out.hashes.insert(path, QString::fromLatin1(hash.result().toHex()));
    else { out.sourceProblems.insert(path, QObject::tr("无法核对源文件")); return false; }
  }
  out.paths << path; out.types.insert(path, type);
  return true;
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
  QString pointsCrs;
  QStringList logPaths;
  const QStringList sources{s.points, s.wells, s.seismic, s.horizons, s.references, s.boundary};
  for (const auto &path : sources) {
    if (path.isEmpty()) continue;
    if (!QFileInfo::exists(path) || !QFileInfo(path).isReadable()) {
      out.sourceProblems.insert(path, QObject::tr("资料路径不存在或不可读")); continue;
    }
    for (const auto &file : collect(path, out, cancelled)) {
      if (cancelled && cancelled()) { out.errors << QObject::tr("已取消扫描"); return out; }
      const QString key = QFileInfo(file).canonicalFilePath();
      if (QFileInfo(s.paleomaps).isDir() && key.startsWith(QFileInfo(s.paleomaps).canonicalFilePath() + '/')) continue;
      if (seen.contains(key)) continue;
      seen.insert(key);
      if (!QFileInfo(file).size()) { out.notes << QObject::tr("空文件已跳过：%1").arg(file); continue; }
      const bool points = QFileInfo(file) == QFileInfo(s.points), boundary = QFileInfo(file) == QFileInfo(s.boundary);
      auto cls = classifyImportFile(file);
      cls.type = suggestedProjectImportType(file, cls.type, boundary ? QStringLiteral("boundary") : QString());
      if (boundary && out.sourceCrs.isEmpty()) out.sourceCrs = paleo::io::readWellTable(file).crs;
      if (points || cls.type == QLatin1String("well_stratification")) {
        if (paleo::io::isWellTableContainer(file) || points) {
          auto table = paleo::io::readWellTable(file, points ? s.pointMapping : QVariantMap{});
          if (points) {
            out.sheets = table.sheets; out.columns = table.columns; out.pointRows = table.preview;
            out.pointMapping = table.mapping; out.sourceCrs = table.crs; out.pointSourceCrs = table.crs;
            if (!table.error.isEmpty() || table.heads.isEmpty() ||
                (!table.crs.isEmpty() && !validNativeCrs(table.crs))) {
              out.sourceProblems.insert(file, table.error.isEmpty() ? QObject::tr("井点无有效记录或源坐标系无效") : table.error);
              out.sourceCrs.clear(); continue;
            }
            // 井点槽里带层位和顶深的表保持分层类型，避免导入时被 forceType 成井口后整文件拒绝。
            cls.type = table.type == QLatin1String("well_stratification")
                           ? table.type : QStringLiteral("well_head");
            out.mappings.insert(file, table.mapping);
            pointsCrs = table.crs;
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
          out.mappings[file].insert("crs", crs);
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
    if (!t.table.error.isEmpty() || t.table.tops.isEmpty()) {
      out.sourceProblems.insert(t.path, t.table.error.isEmpty() ? QObject::tr("分层表没有有效记录") : t.table.error); continue;
    }
    QSet<QString> keys;
    for (const auto &r : t.table.tops) keys.insert(topKey(r));
    if (!keys.isEmpty() && (keys - topRows).isEmpty()) { out.notes << QObject::tr("分层内容重复，已跳过：%1").arg(QFileInfo(t.path).fileName()); continue; }
    if (!appendFile(out, t.path, t.type)) continue;
    out.tops += keys.size(); topRows.unite(keys);
    out.notes << t.table.report.issues;
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
  if (!out.sourceCrs.isEmpty() && !validNativeCrs(out.sourceCrs)) {
    out.notes << QObject::tr("无法识别资料坐标系：%1；未用于工程基准。").arg(out.sourceCrs);
    out.sourceCrs.clear();
  }
  if (out.sourceCrs.isEmpty() && !out.paths.contains(QFileInfo(s.points).absoluteFilePath()) && !out.seismic && !out.horizons)
    for (const auto &map : out.maps) if (map.included && !map.crs.isEmpty()) { out.sourceCrs = map.crs; break; }
  out.projectCrs = out.sourceCrs;
  if (!out.pointRows.isEmpty() && !pointsCrs.isEmpty()) {
    out.projectCrs = metricNativeCrs(pointsCrs, out.pointRows.first().value(1).toDouble(), out.pointRows.first().value(2).toDouble());
    if (!sameNativeCrs(out.sourceCrs, out.projectCrs)) {
      out.notes << QObject::tr("井点将转换到米制工程坐标 %1，原经纬度保留在源表。").arg(out.projectCrs);

    }
  }
  if (s.projectCrs.trimmed().isEmpty() && !out.projectCrs.isEmpty() && !paleo::io::isMetricProjectCrs(out.projectCrs)) {
    const auto reason = QObject::tr("资料坐标系 %1 不能自动选作米制工程基准；请明确工程基准后重试。").arg(out.projectCrs);
    const auto candidates = out.paths;
    for (const auto &path : candidates)
      if (QStringList{"well_head", "horizon", "seismic", "boundary"}.contains(out.types.value(path))) {
        out.sourceProblems.insert(path, reason); out.paths.removeAll(path);
      }
    for (auto &map : out.maps) if (map.included) { map.problem = reason; out.sourceProblems.insert(map.path, reason); }
    out.projectCrs.clear();
  }
  if (!s.projectCrs.trimmed().isEmpty()) out.projectCrs = s.projectCrs.trimmed();
  if (!out.projectCrs.isEmpty() && !paleo::io::isMetricProjectCrs(out.projectCrs))
    out.errors << QObject::tr("工程基准必须为有效的米制投影坐标系：%1").arg(out.projectCrs);
  const QString pointsPath = QFileInfo(s.points).absoluteFilePath();
  if (!s.points.isEmpty() && out.paths.contains(pointsPath) && pointsCrs.isEmpty() && !out.projectCrs.isEmpty()) {
    out.sourceProblems.insert(pointsPath, QObject::tr("请先声明井点资料的源坐标系；工程基准不能代替源坐标系。"));
    out.paths.removeAll(pointsPath); out.types.remove(pointsPath); out.hashes.remove(pointsPath);
    out.wells = 0; out.knownWells.clear();
  }
  for (const auto &path : out.paths) {
    if (out.types.value(path) != "horizon" && out.types.value(path) != "seismic") continue;
    if (out.mappings[path].value("crs").toString().isEmpty()) out.mappings[path].insert("crs", out.sourceCrs);
  }
  if (!out.projectCrs.isEmpty()) out.notes << QObject::tr("工程基准：%1；空间派生资料统一转换，源资料保留。").arg(out.projectCrs);
  if (!s.points.isEmpty() && out.paths.contains(pointsPath) && pointsCrs.isEmpty()) out.notes << QObject::tr("井点坐标系未确定：将按局部工程坐标保存；设置坐标系后可叠加网络底图。");
  if (out.logs > out.wells && out.wells) out.notes << QObject::tr("测井份数多于井数：同井多版资料分别保留，不自动混合曲线。");
  if (!s.points.isEmpty() && !out.paths.contains(pointsPath)) { out.wells = 0; out.knownWells.clear(); }
  out.logs = out.seismic = out.horizons = out.references = 0;
  for (const auto &path : out.paths) {
    const auto type = out.types.value(path);
    if (type == "well_log") ++out.logs;
    else if (type == "seismic") ++out.seismic;
    else if (type == "horizon") ++out.horizons;
    else if (!QStringList{"well_head", "well_stratification", "boundary", "time_depth"}.contains(type)) ++out.references;
  }
  if (cancelled && cancelled()) out.errors << QObject::tr("已取消扫描");
  return out;
}
