// 层：数据
#include "dataexport.h"
#include "datagraph.h"
#include "evidenceindex.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QQueue>
#include <QSaveFile>
#include <QTemporaryDir>

namespace paleo::exporting {
namespace {
bool stopped(const Cancel &cancel) { return cancel && cancel(); }
// QTemporaryDir 的随机段是 6 个字母数字；只认本仓导出/导入暂存根的字面量，
// 不碰用户自己的隐藏目录。根清单须与各创建点保持同口径（B-02：#540 清扫器
// 漏了 .mapbook-/.thickness-/.paleomap-import- 三个现存创建点）。
constexpr int kStaleStagingHours = 24;
bool isStagingRootName(const QString &name) {
  static const QStringList prefixes{".map-products-", ".paleo-map-", ".horizon-export-",
                                    ".paleo-batch-", ".paleo-export-",
                                    ".mapbook-", ".thickness-", ".paleomap-import-"};
  for (const auto &prefix : prefixes) {
    if (!name.startsWith(prefix)) continue;
    const auto suffix = name.mid(prefix.size());
    if (suffix.size() != 6) continue;
    bool alphanumeric = true;
    for (const QChar &ch : suffix) if (!ch.isLetterOrNumber()) { alphanumeric = false; break; }
    if (alphanumeric) return true;
  }
  return false;
}
bool dependency(datagraph::Relation r, bool references) {
  using R = datagraph::Relation;
  return r == R::Derived || r == R::Used || r == R::Generated || r == R::EditBaseline ||
      r == R::Rollback || r == R::Supported || r == R::Constrained || r == R::Contradicted ||
      (references && r == R::References);
}
QString safeName(const QString &name) {
  QString clean = QFileInfo(name).fileName();
  if (!DataCatalog::isSafePathSegment(clean)) clean = QStringLiteral("data");
  return clean;
}
bool copyChecked(const QString &source, const QString &destination, const QString &expected,
                 const Cancel &cancel, const Progress &progress, qint64 &done, qint64 total,
                 QString *sha, QString *error) {
  QFile input(source);
  QSaveFile output(destination);
  output.setDirectWriteFallback(false);
  if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
    *error = QObject::tr("无法读取或写入：%1 → %2").arg(source, destination); return false;
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  QElapsedTimer clock; clock.start();
  while (!input.atEnd()) {
    if (stopped(cancel)) { output.cancelWriting(); return false; }
    const auto bytes = input.read(1024 * 1024);
    if (bytes.isEmpty() && input.error() != QFile::NoError) { *error = input.errorString(); return false; }
    if (output.write(bytes) != bytes.size()) { *error = output.errorString(); return false; }
    hash.addData(bytes); done += bytes.size();
    if (progress && clock.elapsed() >= 50) { progress(done, total, QFileInfo(source).fileName()); clock.restart(); }
  }
  *sha = QString::fromLatin1(hash.result().toHex());
  if (!expected.isEmpty() && sha->compare(expected, Qt::CaseInsensitive) != 0) {
    *error = QObject::tr("文件内容与版本留底 SHA-256 不符：%1").arg(source); return false;
  }
  if (stopped(cancel)) { output.cancelWriting(); return false; }
  if (!output.commit()) { *error = output.errorString(); return false; }
  return true;
}
QStringList companions(const QString &path, QString *error, const QVariantList &manifest = {}) {
  if (!manifest.isEmpty()) {
    QStringList result; const auto directory = QFileInfo(path).absoluteDir();
    for (const auto &entry : manifest) {
      const auto name = entry.toMap().value("file").toString();
      const auto member = directory.filePath(name);
      if (!DataCatalog::isSafePathSegment(name) || !QFileInfo(member).isFile() ||
          QFileInfo(member).canonicalPath() != QFileInfo(path).canonicalPath()) {
        if (error) *error = QObject::tr("图件配套文件缺失或路径不安全：%1").arg(name); return {};
      }
      result << member;
    }
    if (!result.contains(path)) { if (error) *error = QObject::tr("图件配套清单缺少主文件"); return {}; }
    return result;
  }
  const QFileInfo info(path);
  const QString suffix = info.suffix().toLower();
  if (suffix == "vrt") {
    *error = QObject::tr("VRT 引用其他文件；请选择 GeoTIFF 交换格式以导出独立数据"); return {};
  }
  if ((suffix == "gpkg" || suffix == "sqlite") &&
      (QFileInfo(path + "-wal").size() > 0 || QFile::exists(path + "-journal"))) {
    *error = QObject::tr("数据库存在未归档写入，请先保存并关闭写入后导出：%1").arg(path); return {};
  }
  QStringList result{path};
  if (suffix == "shp") {
    const QStringList required{"shp", "shx", "dbf"};
    QSet<QString> found;
    for (const auto &file : info.absoluteDir().entryInfoList(QDir::Files)) {
      if (file.completeBaseName().compare(info.completeBaseName(), Qt::CaseInsensitive) == 0 &&
          QStringList{"shp", "shx", "dbf", "prj", "cpg", "qix", "sbn", "sbx"}.contains(file.suffix().toLower())) {
        found.insert(file.suffix().toLower());
        if (!result.contains(file.absoluteFilePath())) result << file.absoluteFilePath();
      }
    }
    for (const auto &ext : required) if (!found.contains(ext)) {
      *error = QObject::tr("Shapefile 缺少组成文件：%1").arg(ext); return {};
    }
  }
  for (const auto &extra : QStringList{path + ".aux.xml", path + ".ovr", path + ".qml",
       info.absoluteDir().filePath(info.completeBaseName() + ".qml"),
       info.absoluteDir().filePath(info.completeBaseName() + ".prj"),
       info.absoluteDir().filePath(info.completeBaseName() + ".csvt"),
       info.absoluteDir().filePath(info.completeBaseName() + ".tfw")})
    if (QFile::exists(extra) && !result.contains(extra)) result << extra;
  return result;
}
}

void sweepStaleStagingRoots(const QString &parentDirectory) {
  if (parentDirectory.isEmpty()) return;
  const QDir parent(parentDirectory);
  if (!parent.exists()) return;
  const auto now = QDateTime::currentDateTime();
  for (const auto &entry : parent.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
    if (!isStagingRootName(entry.fileName())) continue;
    const auto ageHours = entry.lastModified().secsTo(now) / 3600.0;
    if (ageHours > kStaleStagingHours) {
      if (QDir(entry.absoluteFilePath()).removeRecursively())
        qWarning("exporting: swept stale staging root: %s", qPrintable(entry.absoluteFilePath()));
      else
        qWarning("exporting: stale staging root not removable (in use?): %s", qPrintable(entry.absoluteFilePath()));
    } else {
      qWarning("exporting: staging root residue %.1fh old (< %dh, kept): %s", ageHours, kStaleStagingHours,
               qPrintable(entry.absoluteFilePath()));
    }
  }
}

QHash<QString, QStringList> versionIdsForPaths(const DataCatalog &catalog, const QStringList &paths) {
  const QDir base(catalog.projectDir());
  const auto normalized = [&base](const QString &path) {
    auto key = QDir::cleanPath(QDir::isAbsolutePath(path) ? path : base.absoluteFilePath(path));
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
  };
  QHash<QString, QStringList> originals, result;
  const QSet<QString> uniquePaths(paths.cbegin(), paths.cend());
  for (const auto &path : uniquePaths) if (!path.isEmpty()) originals[normalized(path)].append(path);
  for (const auto &version : catalog.versions()) {
    if (version.path.isEmpty()) continue;
    for (const auto &source : originals.value(normalized(version.path))) result[source].append(version.id);
  }
  return result;
}

Plan plan(const DataCatalog &snapshot, const QString &projectDir, const DataRequest &request, const Cancel &cancel) {
  Plan result; result.request = request;
  if (request.scope == Scope::Original && request.format != DataFormat::Native) {
    result.failures << QObject::tr("原始文件导出必须保持原格式"); return result;
  }
  if (request.versionIds.isEmpty()) { result.failures << QObject::tr("未选择数据版本"); return result; }
  QSet<QString> versions, seeds(request.versionIds.cbegin(), request.versionIds.cend());
  if (request.scope == Scope::Sources) {
    const auto index = datagraph::buildIndex(snapshot, {}, cancel, {});
    if (!index || stopped(cancel)) return result;
    QSet<QString> visited;
    QQueue<QString> pending;
    for (const auto &version : request.versionIds) pending.enqueue(datagraph::versionKey(version));
    while (!pending.isEmpty() && !stopped(cancel)) {
      const auto key = pending.dequeue();
      if (visited.contains(key)) continue;
      visited.insert(key);
      if (!index->nodes.contains(key)) {
        result.failures << QObject::tr("来源版本缺失：%1").arg(key); continue;
      }
      const auto node = index->nodes.value(key);
      if (!node.versionId.isEmpty()) {
        versions.insert(node.versionId);
        if (node.cycle) result.failures << QObject::tr("来源关系存在循环：%1").arg(node.versionId);
      }
      int inputs = 0;
      const auto version = snapshot.versionById(node.versionId);
      if (!version.id.isEmpty() && version.stage != "RAW" &&
          QFileInfo(version.fileName.isEmpty() ? version.path : version.fileName).suffix().compare("gpkg", Qt::CaseInsensitive) == 0 &&
          version.extra.value("layer_type", "vector").toString() == "vector") {
        const auto evidence = evidenceindex::readVersionEvidence(version, DataCatalog::resolvedVersionPath(projectDir, version), cancel);
        if (!evidence.error.isEmpty()) result.failures << version.id + "：" + evidence.error;
        if (evidence.corruptRecords || evidence.truncated)
          result.failures << QObject::tr("%1 的图斑证据损坏或读取被截断，无法证明源数据完整").arg(version.id);
        for (const auto &entry : evidence.entries) {
          if (entry.unlinked || entry.sourceVersion.isEmpty()) continue;
          if (entry.role == "REFERENCES" && !request.includeReferences) continue;
          ++inputs;
          result.relations.append(QVariantMap{{"source", datagraph::versionKey(entry.sourceVersion)},
              {"target", key}, {"relation", entry.role}, {"provenance", "gpkg:facies_evidence#" + entry.evidenceId},
              {"feature_uid", entry.featureUid}});
          pending.enqueue(datagraph::versionKey(entry.sourceVersion));
        }
      }
      QSet<QString> mapInputs;
      QStringList previousInputs;
      bool firstExport = true;
      for (const auto &entry : version.extra.value("map_exports").toList()) {
        auto ids = entry.toMap().value("inputs").toStringList(); ids.removeDuplicates(); ids.sort();
        if (!firstExport && ids != previousInputs)
          result.failures << QObject::tr("%1 有多个来源不同的图件导出记录，请使用对应批次的 metadata.json 核对来源").arg(version.id);
        previousInputs = ids; firstExport = false;
        for (const auto &id : ids) mapInputs.insert(id);
      }
      for (const auto &id : mapInputs) {
        ++inputs;
        result.relations.append(QVariantMap{{"source", datagraph::versionKey(id)}, {"target", key},
            {"relation", "CartographicInput"}, {"provenance", "catalog:extra.map_exports"}});
        pending.enqueue(datagraph::versionKey(id));
      }
      for (const int edgeId : index->incoming.value(key)) {
        const auto &edge = index->edges[edgeId];
        if (!dependency(edge.relation, request.includeReferences)) continue;
        ++inputs;
        result.relations.append(QVariantMap{{"source", edge.source}, {"target", edge.target},
            {"relation", datagraph::relationName(edge.relation)}, {"provenance", edge.provenance}});
        pending.enqueue(edge.source);
      }
      if (!node.versionId.isEmpty() && node.kind != datagraph::Kind::Missing && node.stage != "RAW" && inputs == 0)
        result.failures << QObject::tr("版本 %1 未记录制作来源，无法证明源数据完整").arg(node.versionId);
    }
  } else versions = seeds;
  QStringList ordered = versions.values(); ordered.sort();
  QSet<QString> assetIds;
  for (const auto &id : ordered) {
    if (stopped(cancel)) break;
    const auto version = snapshot.versionById(id);
    if (version.id.isEmpty()) { result.failures << QObject::tr("来源版本缺失：%1").arg(id); continue; }
    if (request.scope == Scope::Original && version.stage != "RAW") {
      result.failures << QObject::tr("%1 不是 RAW 原始版本").arg(id); continue;
    }
    const auto asset = snapshot.assetById(version.assetId);
    const auto path = DataCatalog::resolvedVersionPath(projectDir, version);
    if (asset.id.isEmpty() || path.isEmpty() || !QFileInfo(path).isFile()) {
      result.failures << QObject::tr("%1：文件缺失或路径不可读取").arg(id); continue;
    }
    QString preparationError;
    const auto parts = companions(path, &preparationError, version.extra.value("bundle_members").toList());
    if (!preparationError.isEmpty()) { result.failures << id + "：" + preparationError; continue; }
    const auto category = seeds.contains(id) ? QStringLiteral("data") : QStringLiteral("sources");
    result.items.append({asset, version, path, category}); assetIds.insert(asset.id);
    for (const auto &part : parts) result.bytes += QFileInfo(part).size();
    if (version.sha256.isEmpty()) result.warnings << QObject::tr("%1 无历史摘要，本次将计算 SHA-256，但不能验证入库时内容").arg(id);
  }
  QSet<QString> entities;
  for (const auto &link : snapshot.links()) if (assetIds.contains(link.assetId)) {
    result.links.append(QVariantMap{{"asset_id", link.assetId}, {"entity_id", link.entityId},
        {"role", link.role}, {"primary", link.isPrimary}});
    entities.insert(link.entityId);
  }
  for (const auto &id : entities) {
    const auto entity = snapshot.entityById(id);
    if (!entity.id.isEmpty()) result.entities.append(QVariantMap{{"id", entity.id}, {"name", entity.name},
        {"type", entity.entityType}, {"has_surface", entity.hasSurface},
        {"surface_x", entity.surfaceX}, {"surface_y", entity.surfaceY}});
  }
  result.failures.removeDuplicates();
  return result;
}

Result execute(const Plan &plan, const Cancel &cancel, const Progress &progress, const Convert &converter) {
  Result result; result.failures = plan.failures; result.warnings = plan.warnings;
  result.total = plan.items.size() + plan.failures.size();
  const QFileInfo destination(plan.request.destination);
  if (plan.request.destination.isEmpty() || destination.exists() || !destination.absoluteDir().exists()) {
    result.error = QObject::tr("导出目标必须是已有父目录中的新目录"); return result;
  }
  sweepStaleStagingRoots(destination.absoluteDir().path()); // 崩溃残留的 .paleo-export-*（#540）
  QTemporaryDir staging(destination.absoluteDir().filePath(".paleo-export-XXXXXX"));
  if (!staging.isValid()) { result.error = QObject::tr("无法创建导出暂存目录"); return result; }
  QVariantList manifest;
  qint64 done = 0;
  for (const auto &item : plan.items) {
    if (stopped(cancel)) { result.cancelled = true; return result; }
    const auto &v = item.version;
    if (!DataCatalog::isSafePathSegment(item.asset.id) || !DataCatalog::isSafePathSegment(v.id)) {
      result.failures << QObject::tr("资产或版本 ID 不是安全路径段"); continue;
    }
    const QString relative = item.category + "/" + item.asset.id + "/" + v.id;
    const auto directory = staging.filePath(relative);
    QDir().mkpath(directory);
    QString error, sourceSha;
    QStringList files;
    QVariantMap description;
    // Pin and verify bytes before handing the private copy to a format writer.
    // File backed layers are never reconstructed from a mutable live URI.
    QTemporaryDir input(staging.filePath(".input-XXXXXX"));
    if (!input.isValid()) { result.error = QObject::tr("无法创建导出暂存目录"); return result; }
    const auto parts = companions(item.path, &error, v.extra.value("bundle_members").toList());
    QStringList inputs = parts;
    if (QFileInfo(item.path).suffix().compare("vrt", Qt::CaseInsensitive) == 0 && plan.request.format != DataFormat::Native) {
      // VRT cannot be made self-contained by copying the declaration alone.
      error = QObject::tr("VRT 外部依赖暂不能冻结，请先保存为 GeoTIFF 再导出");
    }
    for (const auto &part : inputs) {
      const auto target = input.filePath(QFileInfo(part).fileName());
      QString digest;
      QString expected = part == item.path ? v.sha256 : QString();
      for (const auto &entry : v.extra.value("bundle_members").toList())
        if ((part != item.path || expected.isEmpty()) && entry.toMap().value("file").toString() == QFileInfo(part).fileName()) expected = entry.toMap().value("sha256").toString();
      if (!copyChecked(part, target, expected, cancel, progress,
                       done, plan.bytes, &digest, &error)) break;
      if (part == item.path) sourceSha = digest;
    }
    if (stopped(cancel)) { result.cancelled = true; return result; }
    const bool convert = plan.request.format != DataFormat::Native &&
        !(plan.request.scope == Scope::Sources && !plan.request.versionIds.contains(v.id));
    if (error.isEmpty() && convert) {
      if (QFileInfo(item.path).suffix().compare("csv", Qt::CaseInsensitive) == 0 &&
          (plan.request.format == DataFormat::Exchange || plan.request.format == DataFormat::Csv)) {
        for (const auto &part : inputs) {
          const auto name = safeName(part);
          const auto path = QDir(directory).filePath(name);
          // 暂存内键与落盘名解耦（#540）：写入暂存时用的键是
          // QFileInfo(part).fileName()（与上面拷贝入暂存同键），safeName 只作用
          // 于最终落盘名——否则段名不合法时这里必然假失败。
          if (QFile::copy(input.filePath(QFileInfo(part).fileName()), path)) files << path;
          else { error = QObject::tr("无法写出 CSV 及配套文件"); break; }
        }
      } else if (!converter) error = QObject::tr("该格式的写出服务尚未连接");
      else if (!converter(input.filePath(QFileInfo(item.path).fileName()), directory,
                     plan.request.format, &files, &description, &error, cancel) && error.isEmpty())
        error = QObject::tr("交换格式写出失败");
    } else if (error.isEmpty()) {
      for (const auto &part : inputs) {
        const auto path = QDir(directory).filePath(QFileInfo(part).fileName());
        if (!QFile::copy(input.filePath(QFileInfo(part).fileName()), path)) { error = QObject::tr("无法提交数据文件"); break; }
        files << path;
      }
    }
    if (stopped(cancel)) { result.cancelled = true; return result; }
    if (!error.isEmpty()) { result.failures << v.id + "：" + error; QDir(directory).removeRecursively(); continue; }
    QVariantList outputs;
    for (const auto &file : files) {
      const auto rel = QDir(staging.path()).relativeFilePath(file);
      const auto hash = DataCatalog::sha256FileHex(file, &error, cancel);
      if (stopped(cancel)) { result.cancelled = true; return result; }
      if (hash.isEmpty()) break;
      outputs.append(QVariantMap{{"path", rel}, {"sha256", hash}, {"bytes", QFileInfo(file).size()}});
      result.files << rel;
    }
    if (!error.isEmpty()) { result.error = error; return result; }
    if (description.contains("warning")) result.warnings << description.value("warning").toString();
    manifest.append(QVariantMap{{"asset_id", item.asset.id}, {"version_id", v.id},
        {"name", item.asset.displayName}, {"stage", v.stage}, {"version", v.versionNumber},
        {"source_sha256", sourceSha}, {"parent_version_ids", v.parentVersionIds},
        {"metadata", v.extra}, {"description", description}, {"files", outputs}});
    ++result.succeeded;
  }
  if (stopped(cancel)) { result.cancelled = true; return result; }
  const auto metadata = QJsonDocument::fromVariant(QVariantMap{{"schema", "paleo_export_v1"},
      {"exported_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
      {"scope", int(plan.request.scope)}, {"format", int(plan.request.format)},
      {"root_versions", plan.request.versionIds}, {"complete", result.complete()},
      {"assets", manifest}, {"relations", plan.relations}, {"entities", plan.entities}, {"links", plan.links},
      {"failures", result.failures}, {"warnings", result.warnings}}).toJson(QJsonDocument::Indented);
  QSaveFile file(staging.filePath("metadata.json"));
  if (!file.open(QIODevice::WriteOnly) || file.write(metadata) != metadata.size() || !file.commit()) {
    result.error = QObject::tr("导出说明写入失败"); return result;
  }
  QFile readme(staging.filePath("README.txt"));
  const auto text = QObject::tr("Paleo 数据导出\n具体版本、来源关系、字段/单位与摘要见 metadata.json。\n成功 %1 项；问题 %2 项。\n数值 GeoTIFF 保留数据值；地质图件另在图件导出入口输出。\n")
      .arg(result.succeeded).arg(result.failures.size()).toUtf8();
  if (!readme.open(QIODevice::WriteOnly) || readme.write(text) != text.size()) {
    result.error = QObject::tr("导出说明写入失败"); return result;
  }
  readme.close();
  if (stopped(cancel)) { result.cancelled = true; return result; }
  if (!QDir().rename(staging.path(), destination.absoluteFilePath())) {
    result.error = QObject::tr("无法提交导出目录，目标可能已存在"); return result;
  }
  result.directory = destination.absoluteFilePath();
  result.files << "metadata.json" << "README.txt";
  for (auto &path : result.files) path = QDir(result.directory).filePath(path);
  if (progress) progress(done, done, QObject::tr("导出完成"));
  return result;
}
}
