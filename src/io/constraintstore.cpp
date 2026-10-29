// 层：数据
#include "constraintstore.h"
#include "gdalreg_internal.h"
#include "catalog/datacatalog.h"
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>
#include <cpl_error.h>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <cmath>
#include <memory>

namespace {
using paleo::io_detail::ensureGdalRegistered;
using Feature = std::unique_ptr<void, decltype(&OGR_F_Destroy)>;
using Geometry = std::unique_ptr<void, decltype(&OGR_G_DestroyGeometry)>;
QString freshId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString localActor() {
  auto name = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", "operator")).trimmed();
  return QStringLiteral("local:%1").arg(name.isEmpty() ? QStringLiteral("operator") : name);
}
QString quoteSql(QString value) { return "'" + value.replace("'", "''") + "'"; }
bool ensureField(OGRLayerH layer, const char *name, OGRFieldType type) {
  if (OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(layer), name) >= 0) return true;
  auto field = OGR_Fld_Create(name, type);
  const auto result = OGR_L_CreateField(layer, field, TRUE);
  OGR_Fld_Destroy(field);
  return result == OGRERR_NONE;
}
bool sql(GDALDatasetH ds, const QByteArray &command) {
  CPLErrorReset(); auto result = GDALDatasetExecuteSQL(ds, command.constData(), nullptr, "SQLITE");
  if (result) GDALDatasetReleaseResultSet(ds, result);
  return CPLGetLastErrorType() < CE_Failure;
}
QString parameterFingerprint(const QVariantMap &row) {
  const auto raw = row.value("params_json").toString().toUtf8();
  QJsonParseError error; const auto parsed = QJsonDocument::fromJson(raw, &error);
  // Preserve invalid legacy input as an explicitly raw fingerprint; never
  // silently assign it the meaning of an empty scientific parameter object.
  const QByteArray canonical = raw.trimmed().isEmpty() ? QByteArray("absent") :
      error.error == QJsonParseError::NoError ? parsed.toJson(QJsonDocument::Compact) : QByteArray("invalid:") + raw;
  const auto schema = QByteArray::number(row.value("schema_version").toInt());
  return QString::fromLatin1(QCryptographicHash::hash(schema + ':' + canonical, QCryptographicHash::Sha256).toHex());
}
QVariantMap featureSnapshot(OGRFeatureH feature) {
  if (!feature) return {};
  QVariantMap row; auto def = OGR_F_GetDefnRef(feature);
  for (int i = 0; i < OGR_FD_GetFieldCount(def); ++i) {
    auto field = OGR_FD_GetFieldDefn(def, i);
    const auto name = QString::fromUtf8(OGR_Fld_GetNameRef(field));
    if (name == QLatin1String("evidence_revision") || name == QLatin1String("evidence_recent_events")) continue;
    if (!OGR_F_IsFieldSetAndNotNull(feature, i)) { row.insert(name, QVariant()); continue; }
    switch (OGR_Fld_GetType(field)) {
      case OFTInteger: row.insert(name, OGR_F_GetFieldAsInteger(feature, i)); break;
      case OFTInteger64: row.insert(name, qlonglong(OGR_F_GetFieldAsInteger64(feature, i))); break;
      case OFTReal: row.insert(name, OGR_F_GetFieldAsDouble(feature, i)); break;
      default: row.insert(name, QString::fromUtf8(OGR_F_GetFieldAsString(feature, i))); break;
    }
  }
  auto geometry = OGR_F_GetGeometryRef(feature);
  if (geometry) {
    OGRWktOptions options(17, false); options.format = OGRWktFormat::G;
    row.insert("wkt", QString::fromStdString(OGRGeometry::FromHandle(geometry)->exportToWkt(options)));
    QByteArray wkb(OGR_G_WkbSize(geometry), '\0');
    OGR_G_ExportToWkb(geometry, wkbNDR, reinterpret_cast<unsigned char *>(wkb.data()));
    row.insert("geometry_fingerprint", QString::fromLatin1(QCryptographicHash::hash(wkb, QCryptographicHash::Sha256).toHex()));
  }
  row.insert("params_fingerprint", parameterFingerprint(row));
  return row;
}
bool sameBusinessState(QVariantMap before, QVariantMap after) {
  // Whitespace/key order in a JSON object is not a new scientific parameter.
  before.remove("params_json"); after.remove("params_json");
  return before == after;
}
// L-26：fidBelow >= 0 时走游标翻页（AND fid < N 且 OFFSET 恒 0，minFidOut
// 回传页内最小 fid）——OFFSET 翻页对并发 append 不稳定：新事件 fid 插到
// 前面使后续页窗口前移，整页事件被跳过、审计链出现空洞。
QVector<QVariantMap> readHistory(GDALDatasetH ds, const QString &column, const QString &key,
                                 int limit, int offset, QString *error,
                                 qint64 fidBelow = -1, qint64 *minFidOut = nullptr) {
  QVector<QVariantMap> result;
  if (!GDALDatasetGetLayerByName(ds, "constraint_audit")) return result;
  // #441：列名白名单固定两条语句；键用 quoteSql 字面量，limit/offset 用十进制数字拼接。
  // 禁止对已拼好的 SQL 再调 QString::arg，否则键里的 %1/%3 会污染 LIMIT/OFFSET。
  const QString cursor = fidBelow >= 0
                             ? QStringLiteral(" AND fid < ") + QString::number(fidBelow)
                             : QString();
  const QString offsetClause = fidBelow >= 0 ? QStringLiteral("0") : QString::number(qMax(0, offset));
  // fid 按名取 + 层 FID 兜底：GDAL SQLite 方言对 GPKG 层的 fid 列有特殊
  // 映射——实测（GDAL 3.13）裸列名与「fid AS audit_fid」别名都会被从结果
  // 字段表剥离、改为承载在层 FID 上，固定列序按位读会把 fid 值当
  // event_json，整表行被判损坏跳过、审计历史恒空（#815 引入的回归）。
  // 按名取 event_json；audit_fid 不在场时以 OGR_F_GetFID 取回同一值。
  QString query;
  if (column == QLatin1String("constraint_id")) {
    query = QStringLiteral("SELECT fid AS audit_fid, event_json FROM constraint_audit WHERE constraint_id=")
        + quoteSql(key) + cursor
        + QStringLiteral(" ORDER BY fid DESC LIMIT ")
        + QString::number(qBound(1, limit, 200))
        + QStringLiteral(" OFFSET ")
        + offsetClause;
  } else if (column == QLatin1String("horizon")) {
    query = QStringLiteral("SELECT fid AS audit_fid, event_json FROM constraint_audit WHERE horizon=")
        + quoteSql(key) + cursor
        + QStringLiteral(" ORDER BY fid DESC LIMIT ")
        + QString::number(qBound(1, limit, 200))
        + QStringLiteral(" OFFSET ")
        + offsetClause;
  } else {
    if (error) *error = QObject::tr("约束审计查询列无效");
    return result;
  }
  const auto sql = query.toUtf8();
  CPLErrorReset(); auto rows = GDALDatasetExecuteSQL(ds, sql.constData(), nullptr, "SQLITE");
  if (!rows) { if (error) *error = QString::fromUtf8(CPLGetLastErrorMsg()); return result; }
  auto *defn = OGR_L_GetLayerDefn(rows);
  const int jsonIdx = OGR_FD_GetFieldIndex(defn, "event_json");
  const int fidIdx = OGR_FD_GetFieldIndex(defn, "audit_fid");
  if (jsonIdx < 0) {
    GDALDatasetReleaseResultSet(ds, rows);
    if (error) *error = QObject::tr("约束审计结果列缺失");
    return result;
  }
  while (auto raw = OGR_L_GetNextFeature(rows)) {
    Feature feature(raw, OGR_F_Destroy); QJsonParseError parse;
    // SQLite 方言把 GPKG 的 fid 列映射进层 FID（audit_fid 字段被剥离）：
    // 字段在场则按名读，缺席时 GetFID 即同一 fid——两代方言行为都正确。
    const qint64 fid = fidIdx >= 0 ? OGR_F_GetFieldAsInteger64(raw, fidIdx)
                                   : OGR_F_GetFID(raw);
    const auto rawBytes = QByteArray(OGR_F_GetFieldAsString(raw, jsonIdx));
    auto document = QJsonDocument::fromJson(rawBytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
      qWarning() << "[ConstraintStore] Corrupted audit event row skipped:" << rawBytes;
      continue;
    }
    result << document.toVariant().toMap();
    if (minFidOut && ( *minFidOut == 0 || fid < *minFidOut ))
      *minFidOut = fid; // 事件图本身不掺 fid（快照 event_json 保持原样）
  }
  GDALDatasetReleaseResultSet(ds, rows); return result;
}
bool ensureAuditTable(GDALDatasetH ds) {
  auto audit = GDALDatasetGetLayerByName(ds, "constraint_audit");
  const bool needsIndex = (audit == nullptr);
  if (!audit) audit = GDALDatasetCreateLayer(ds, "constraint_audit", nullptr, wkbNone, nullptr);
  if (!audit) return false;
  for (const auto name : {"event_id", "constraint_id", "horizon", "operation_id", "actor_id", "action", "timestamp", "event_json"})
    if (!ensureField(audit, name, OFTString)) return false;
  if (!ensureField(audit, "revision", OFTInteger64)) return false;
  if (needsIndex) {
    return sql(ds, "CREATE UNIQUE INDEX IF NOT EXISTS paleo_constraint_event_id ON constraint_audit(event_id)") &&
           sql(ds, "CREATE INDEX IF NOT EXISTS paleo_constraint_history_id ON constraint_audit(constraint_id,fid)") &&
           sql(ds, "CREATE INDEX IF NOT EXISTS paleo_constraint_history_horizon ON constraint_audit(horizon,fid)");
  }
  return true;
}
bool ensureAudit(GDALDatasetH ds, OGRLayerH constraints) {
  if (!ensureAuditTable(ds) || !constraints) return false;
  const bool needsIndex = (OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(constraints), "evidence_revision") < 0);
  if (!ensureField(constraints, "evidence_revision", OFTInteger64) ||
      !ensureField(constraints, "evidence_recent_events", OFTString)) return false;
  if (needsIndex) {
    return sql(ds, "CREATE INDEX IF NOT EXISTS paleo_constraint_stable_id ON constraints(id)") &&
           sql(ds, "CREATE INDEX IF NOT EXISTS paleo_constraint_horizon ON constraints(horizon)");
  }
  return true;
}
bool journal(GDALDatasetH ds, OGRLayerH constraints, OGRFeatureH afterFeature,
              const QVariantMap &before, const QVariantMap &after,
              const ConstraintStore::AuditContext &context, const QString &operation, QString *error) {
  if (sameBusinessState(before, after)) return true;
  if (!ensureAudit(ds, constraints)) { *error = QObject::tr("无法建立约束审计表"); return false; }
  const auto target = after.isEmpty() ? before : after; const auto id = target.value("id").toString();
  QString historyError; const auto recent = readHistory(ds, "constraint_id", id, 3, 0, &historyError);
  if (!historyError.isEmpty()) { *error = historyError; return false; }
  const auto previous = recent.isEmpty() ? 0 : recent.first().value("after_revision").toLongLong();
  const auto change = before.isEmpty() ? "create" : after.isEmpty() ? "delete" : "update";
  QVariantMap event{{"event_id", freshId()}, {"target_object_id", id}, {"target_object_type", "constraint"},
      {"horizon", target.value("horizon")}, {"operation_id", operation},
      {"actor_id", context.actorId.isEmpty() ? localActor() : context.actorId},
      {"identity_kind", "local_operator"}, {"reason", context.reason.isEmpty() ? QObject::tr("约束业务修改") : context.reason},
      {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"action", context.action.isEmpty() ? QString(change) : context.action}, {"change", change},
      {"before_revision", previous}, {"after_revision", previous + 1},
      {"before", before}, {"after", after}, {"deleted", after.isEmpty()},
      {"before_params_fingerprint", before.value("params_fingerprint")},
      {"after_params_fingerprint", after.value("params_fingerprint")}};
  auto audit = GDALDatasetGetLayerByName(ds, "constraint_audit");
  Feature feature(OGR_F_Create(OGR_L_GetLayerDefn(audit)), OGR_F_Destroy);
  const auto write = [&](const char *name, const QString &value) {
    OGR_F_SetFieldString(feature.get(), OGR_F_GetFieldIndex(feature.get(), name), value.toUtf8().constData());
  };
  write("event_id", event.value("event_id").toString()); write("constraint_id", id);
  write("horizon", target.value("horizon").toString()); write("operation_id", operation);
  write("actor_id", event.value("actor_id").toString()); write("action", event.value("action").toString());
  write("timestamp", event.value("timestamp").toString());
  write("event_json", QString::fromUtf8(QJsonDocument::fromVariant(event).toJson(QJsonDocument::Compact)));
  OGR_F_SetFieldInteger64(feature.get(), OGR_F_GetFieldIndex(feature.get(), "revision"), previous + 1);
  if (OGR_L_CreateFeature(audit, feature.get()) != OGRERR_NONE) { *error = QObject::tr("约束审计追加失败"); return false; }
  if (afterFeature) {
    // DDL extends the layer definition after afterFeature was created. Reload
    // the newly durable row before writing metadata, preserving scalar fields.
    Feature updated(OGR_L_GetFeature(constraints, OGR_F_GetFID(afterFeature)), OGR_F_Destroy);
    if (!updated) { *error = QObject::tr("约束审计目标不可用"); return false; }
    QVariantList brief{event};
    for (const auto &past : recent) { if (brief.size() == 3) break; brief << past; }
    OGR_F_SetFieldInteger64(updated.get(), OGR_F_GetFieldIndex(updated.get(), "evidence_revision"), previous + 1);
    OGR_F_SetFieldString(updated.get(), OGR_F_GetFieldIndex(updated.get(), "evidence_recent_events"),
        QJsonDocument::fromVariant(brief).toJson(QJsonDocument::Compact).constData());
    if (OGR_L_SetFeature(constraints, updated.get()) != OGRERR_NONE) { *error = QObject::tr("约束审计修订登记失败"); return false; }
  }
  return true;
}
struct Transaction {
  GDALDatasetH ds = nullptr; QString path; bool created = false, committed = false;
  ~Transaction() {
    if (ds) { if (!committed) GDALDatasetRollbackTransaction(ds); GDALClose(ds); }
    if (created && !committed) QFile::remove(path);
  }
  bool open(const QString &file, bool allowCreate, QString *error) {
    ensureGdalRegistered(); path = file;
    if (!QFile::exists(file) && !allowCreate) {
      if (error) *error = QStringLiteral("Constraint file does not exist: ") + file;
      return false;
    }
    if (QFile::exists(file)) ds = GDALOpenEx(file.toUtf8().constData(), GDAL_OF_UPDATE | GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    else if (allowCreate && QDir().mkpath(QFileInfo(file).absolutePath())) {
      const auto driver = GDALGetDriverByName("GPKG");
      if (driver) ds = GDALCreate(driver, file.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
      created = ds != nullptr;
    }
    if (ds) sql(ds, "PRAGMA busy_timeout = 5000");
    if (!ds || GDALDatasetStartTransaction(ds, FALSE) != OGRERR_NONE) {
      if (error) *error = QObject::tr("无法打开约束事务：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
      return false;
    }
    return true;
  }
  bool commit(QString *error) {
    if (GDALDatasetCommitTransaction(ds) != OGRERR_NONE) { *error = QObject::tr("约束与审计事务提交失败"); return false; }
    committed = true; return true;
  }
};
Feature byId(OGRLayerH layer, const QString &id, QString *error) {
  if (!layer) { *error = QObject::tr("约束图层缺失"); return Feature(nullptr, OGR_F_Destroy); }
  const auto filter = ("id=" + quoteSql(id)).toUtf8();
  if (OGR_L_SetAttributeFilter(layer, filter.constData()) != OGRERR_NONE) {
    *error = QObject::tr("约束身份字段缺失"); return Feature(nullptr, OGR_F_Destroy);
  }
  OGR_L_ResetReading(layer); Feature result(OGR_L_GetNextFeature(layer), OGR_F_Destroy);
  Feature duplicate(OGR_L_GetNextFeature(layer), OGR_F_Destroy); OGR_L_SetAttributeFilter(layer, nullptr);
  if (duplicate) { *error = QObject::tr("约束稳定身份重复，无法确定修改对象"); result.reset(); }
  return result;
}
bool assignGeometry(OGRFeatureH feature, const QString &wkt, QString *error) {
  auto bytes = wkt.toUtf8(); char *text = bytes.data(); OGRGeometryH raw = nullptr;
  const auto parsed = OGR_G_CreateFromWkt(&text, nullptr, &raw); Geometry geometry(raw, OGR_G_DestroyGeometry);
  if (parsed != OGRERR_NONE || !raw || OGR_G_IsEmpty(raw) || OGR_F_SetGeometry(feature, raw) != OGRERR_NONE) {
    *error = QObject::tr("约束几何无效"); return false;
  }
  return true;
}
// P0-2：库约定 CRS（本地工程网格 WKT）→ OGR SRS 句柄；解析失败返回 null。
OGRSpatialReferenceH newStoreSrs() {
  const QByteArray wkt = DataCatalog::localGridCrsWkt().toUtf8();
  char *text = const_cast<char *>(wkt.constData());
  OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
  if (srs && OSRImportFromWkt(srs, &text) != OGRERR_NONE) {
    OSRDestroySpatialReference(srs);
    return nullptr;
  }
  return srs;
}
// 建层即带库约定 CRS：外部 QGIS/GDAL 打开即可独立辨识坐标含义。几何类型维持
// wkbUnknown（GPKG GEOMETRY：混合点线面按伴随视图方案，不改 schema）。库 CRS
// 解析失败时显式失败——绝不静默建出无 SRS 的 legacy 层（V5 复审）。
OGRLayerH createConstraintsLayer(GDALDatasetH ds, QString *error) {
  OGRSpatialReferenceH srs = newStoreSrs();
  if (!srs) {
    if (error)
      *error = QObject::tr("库约定 CRS（本地工程网格）解析失败，拒绝建立无 SRS 约束层");
    return nullptr;
  }
  OGRLayerH layer = GDALDatasetCreateLayer(ds, "constraints", srs, wkbUnknown, nullptr);
  OSRDestroySpatialReference(srs);
  if (!layer && error)
    *error = QObject::tr("无法建立约束图层：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
  return layer;
}
bool contextValid(const ConstraintStore::AuditContext &context, QString *error) {
  if ((!context.actorId.isEmpty() && (!context.actorId.startsWith("local:") || context.actorId.mid(6).trimmed().isEmpty())) ||
      (!context.action.isEmpty() && !QStringList{"edit", "undo", "redo", "cancel", "restore"}.contains(context.action))) {
    if (error) *error = QObject::tr("约束操作人需明确为本地身份，操作类型无效"); return false;
  }
  return true;
}
}
ConstraintStore::ConstraintStore(const QString &gpkgPath, EnqueueFn enqueue)
  : m_gpkgPath(gpkgPath)
  , m_enqueue(std::move(enqueue))
{
}

ConstraintStore::ConstraintStore(const QString &gpkgPath, PaleoProjectStore *store)
  : m_gpkgPath(gpkgPath)
{
  if (store)
  {
    QPointer<PaleoProjectStore> safeStore(store);
    m_enqueue = [safeStore](const WriteFn &fn) -> PaleoProjectStore::WriteResult {
      PaleoProjectStore *storePtr = safeStore.data();
      if (!storePtr)
      {
        return {false, QStringLiteral("PaleoProjectStore destroyed or unavailable")};
      }
      return storePtr->enqueueWrite(fn);
    };
  }
}

bool ConstraintStore::setAuditContext(const AuditContext &context, QString *error) {
  if (!contextValid(context, error)) return false;
  if (!context.actorId.isEmpty() && context.reason.trimmed().isEmpty()) {
    if (error) *error = QObject::tr("明确指定操作人时必须填写业务理由");
    return false;
  }
  m_audit = context; return true;
}
bool ConstraintStore::append(const QString &horizon, const QString &id, const QString &wkt,
    const QString &type, int faciesCode, QString *error, double weight) {
  return appendInternal(horizon, id, wkt, type, faciesCode, nullptr, 0, error, weight);
}
bool ConstraintStore::appendExtended(const QString &horizon, const QString &id, const QString &wkt,
    const QString &type, int faciesCode, const QString &paramsJson, int schemaVersion, QString *error, double weight) {
  return appendInternal(horizon, id, wkt, type, faciesCode, &paramsJson, schemaVersion, error, weight);
}
bool ConstraintStore::ensureSchema(QString *error) {
  if (m_readOnly) { if (error) *error = QObject::tr("已发布约束快照只读"); return false; }
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    QString reason;
    Transaction transaction; if (!transaction.open(m_gpkgPath, true, &reason)) return {false, reason};
    auto layer = GDALDatasetGetLayerByName(transaction.ds, "constraints");
    if (!layer) layer = createConstraintsLayer(transaction.ds, &reason);
    if (!layer) return {false, reason.isEmpty() ? QObject::tr("无法建立约束图层") : reason};
    for (const auto name : {"id", "horizon", "type", "params_json"})
      if (!ensureField(layer, name, OFTString)) return {false, QObject::tr("无法建立约束字段")};
    if (!ensureField(layer, "facies_code", OFTInteger) || !ensureField(layer, "weight", OFTReal) ||
        !ensureField(layer, "schema_version", OFTInteger)) return {false, QObject::tr("无法建立约束字段")};
    if (!ensureAudit(transaction.ds, layer)) return {false, QObject::tr("无法建立约束审计")};
    if (!transaction.commit(&reason)) return {false, reason};
    return {true, {}};
  };
  const auto result = m_enqueue ? m_enqueue(write) : write(); if (!result.ok && error) *error = result.error; return result.ok;
}

bool ConstraintStore::appendInternal(const QString &horizon, const QString &id, const QString &wkt,
    const QString &type, int faciesCode, const QString *paramsJson, int schemaVersion, QString *error, double weight) {
  if (m_readOnly) { if (error) *error = QObject::tr("已发布约束快照只读"); return false; }
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    QString reason;
    if (horizon.trimmed().isEmpty() || id.trimmed().isEmpty() || type.trimmed().isEmpty() || !std::isfinite(weight))
      return {false, QObject::tr("约束身份、层位、语义或权重无效")};
    // V8（复审）：'|' 是 GPKG datasource URI 的参数分隔符——层位名带它会破坏
    // constraints.<层位> 图层源（subset 逃逸之外的第二条破坏路径），入口拒绝。
    if (horizon.contains(QLatin1Char('|')))
      return {false, QObject::tr("层位名不能包含「|」（GPKG 数据源分隔符）：%1").arg(horizon)};
    Transaction transaction; if (!transaction.open(m_gpkgPath, true, &reason)) return {false, reason};
    auto layer = GDALDatasetGetLayerByName(transaction.ds, "constraints");
    if (!layer) layer = createConstraintsLayer(transaction.ds, &reason);
    if (!layer) return {false, reason.isEmpty() ? QObject::tr("无法建立约束图层") : reason};
    for (const auto name : {"id", "horizon", "type", "params_json"})
      if (!ensureField(layer, name, OFTString)) return {false, QObject::tr("无法建立约束字段")};
    if (!ensureField(layer, "facies_code", OFTInteger) || !ensureField(layer, "weight", OFTReal) ||
        !ensureField(layer, "schema_version", OFTInteger)) return {false, QObject::tr("无法建立约束字段")};
    { auto existing = byId(layer, id, &reason); if (!reason.isEmpty()) return {false, reason};
      if (existing) return {false, QObject::tr("约束稳定身份已存在")}; }
    if (!ensureAudit(transaction.ds, layer)) return {false, QObject::tr("无法建立约束审计")};
    Feature feature(OGR_F_Create(OGR_L_GetLayerDefn(layer)), OGR_F_Destroy);
    const auto set = [&](const char *name, const QString &value) {
      OGR_F_SetFieldString(feature.get(), OGR_F_GetFieldIndex(feature.get(), name), value.toUtf8().constData());
    };
    set("id", id); set("horizon", horizon); set("type", type);
    OGR_F_SetFieldInteger(feature.get(), OGR_F_GetFieldIndex(feature.get(), "facies_code"), faciesCode);
    OGR_F_SetFieldDouble(feature.get(), OGR_F_GetFieldIndex(feature.get(), "weight"), weight);
    if (paramsJson) { set("params_json", *paramsJson);
      OGR_F_SetFieldInteger(feature.get(), OGR_F_GetFieldIndex(feature.get(), "schema_version"), schemaVersion); }
    if (!assignGeometry(feature.get(), wkt, &reason) || OGR_L_CreateFeature(layer, feature.get()) != OGRERR_NONE)
      return {false, reason.isEmpty() ? QObject::tr("约束写入失败") : reason};
    if (!journal(transaction.ds, layer, feature.get(), {}, featureSnapshot(feature.get()), m_audit,
          m_audit.operationId.isEmpty() ? freshId() : m_audit.operationId, &reason) || !transaction.commit(&reason)) return {false, reason};
    return {true, {}};
  };
  const auto result = m_enqueue ? m_enqueue(write) : write(); if (!result.ok && error) *error = result.error; return result.ok;
}
QVector<QVariantMap> ConstraintStore::load(const QString &horizon, bool includeAudit) const
{
  ensureGdalRegistered();
  QVector<QVariantMap> result;

  if (!QFile::exists(m_gpkgPath))
    return result;

  GDALDatasetH ds = GDALOpenEx(m_gpkgPath.toUtf8().constData(),
                               GDAL_OF_READONLY | GDAL_OF_VECTOR,
                               nullptr, nullptr, nullptr);
  if (!ds)
    return result;

  OGRLayerH layer = GDALDatasetGetLayerByName(ds, "constraints");
  if (!layer)
  {
    GDALClose(ds);
    return result;
  }

  OGRFeatureDefnH layerDefn = OGR_L_GetLayerDefn(layer);
  int idIdx = OGR_FD_GetFieldIndex(layerDefn, "id");
  int horizonIdx = OGR_FD_GetFieldIndex(layerDefn, "horizon");
  int typeIdx = OGR_FD_GetFieldIndex(layerDefn, "type");
  int faciesIdx = OGR_FD_GetFieldIndex(layerDefn, "facies_code");
  int weightIdx = OGR_FD_GetFieldIndex(layerDefn, "weight");
  int paramsIdx = OGR_FD_GetFieldIndex(layerDefn, "params_json");
  int schemaIdx = OGR_FD_GetFieldIndex(layerDefn, "schema_version");

  if (!horizon.isEmpty())
    OGR_L_SetAttributeFilter(layer, ("horizon=" + quoteSql(horizon)).toUtf8().constData());

  OGR_L_ResetReading(layer);
  OGRFeatureH feat = nullptr;
  while ((feat = OGR_L_GetNextFeature(layer)) != nullptr)
  {
    QString featHorizon;
    if (horizonIdx >= 0)
    {
      const char *hStr = OGR_F_GetFieldAsString(feat, horizonIdx);
      if (hStr)
        featHorizon = QString::fromUtf8(hStr);
    }

    if (!horizon.isEmpty() && featHorizon != horizon)
    {
      OGR_F_Destroy(feat);
      continue;
    }

    QVariantMap map;
    map.insert(QStringLiteral("fid"), static_cast<qlonglong>(OGR_F_GetFID(feat)));
    if (idIdx >= 0)
    {
      const char *idStr = OGR_F_GetFieldAsString(feat, idIdx);
      map.insert(QStringLiteral("id"), idStr ? QString::fromUtf8(idStr) : QString());
    }
    else
    {
      map.insert(QStringLiteral("id"), QString());
    }

    map.insert(QStringLiteral("horizon"), featHorizon);

    if (typeIdx >= 0)
    {
      const char *typeStr = OGR_F_GetFieldAsString(feat, typeIdx);
      map.insert(QStringLiteral("type"), typeStr ? QString::fromUtf8(typeStr) : QString());
    }
    else
    {
      map.insert(QStringLiteral("type"), QString());
    }

    int fc = (faciesIdx >= 0) ? OGR_F_GetFieldAsInteger(feat, faciesIdx) : 0;
    map.insert(QStringLiteral("facies_code"), fc);
    map.insert(QStringLiteral("target_facies_code"), fc);

    double wt = (weightIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, weightIdx))
                    ? OGR_F_GetFieldAsDouble(feat, weightIdx)
                    : 1.0;
    map.insert(QStringLiteral("weight"), wt);

    if (paramsIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, paramsIdx))
    {
      const char *params = OGR_F_GetFieldAsString(feat, paramsIdx);
      map.insert(QStringLiteral("params_json"), params ? QString::fromUtf8(params) : QString());
    }
    if (schemaIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, schemaIdx))
      map.insert(QStringLiteral("schema_version"), OGR_F_GetFieldAsInteger(feat, schemaIdx));

    QString wktStr;
    OGRGeometryH geom = OGR_F_GetGeometryRef(feat);
    if (geom)
    {
      OGRWktOptions options(17, false);
      options.format = OGRWktFormat::G; // round-trip every binary64 coordinate
      wktStr = QString::fromStdString(OGRGeometry::FromHandle(geom)->exportToWkt(options));
    }
    map.insert(QStringLiteral("wkt"), wktStr);

    if (includeAudit) {
      const int revision = OGR_FD_GetFieldIndex(layerDefn, "evidence_revision");
      const int recent = OGR_FD_GetFieldIndex(layerDefn, "evidence_recent_events");
      map.insert("revision", revision < 0 ? 0 : OGR_F_GetFieldAsInteger64(feat, revision));
      map.insert("params_fingerprint", parameterFingerprint(map));
      QJsonParseError parse;
      const auto raw = recent < 0 ? QByteArray() : QByteArray(OGR_F_GetFieldAsString(feat, recent));
      const auto document = QJsonDocument::fromJson(raw, &parse);
      map.insert("recent_events", document.toVariant().toList());
      if (!raw.isEmpty() && (parse.error != QJsonParseError::NoError || !document.isArray()))
        map.insert("history_error", QObject::tr("约束近期审计快照损坏，请读取完整历史核验"));
      map.insert("history_available", GDALDatasetGetLayerByName(ds, "constraint_audit") != nullptr);
    }
    result.append(map);
    OGR_F_Destroy(feat);
  }

  GDALClose(ds);
  return result;
}

QVector<QVariantMap> ConstraintStore::history(const QString &id, int limit, int offset, QString *error) const {
  ensureGdalRegistered(); if (!QFile::exists(m_gpkgPath)) return {};
  std::unique_ptr<void, decltype(&GDALClose)> ds(GDALOpenEx(m_gpkgPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr), GDALClose);
  if (!ds) { if (error) *error = QObject::tr("约束历史资产不可读取"); return {}; }
  return readHistory(ds.get(), "constraint_id", id, limit, offset, error);
}
QVector<QVariantMap> ConstraintStore::historyForHorizon(const QString &horizon, int limit, int offset, QString *error,
                                                        qint64 fidBelow, qint64 *minFidOut) const {
  ensureGdalRegistered(); if (!QFile::exists(m_gpkgPath)) return {};
  std::unique_ptr<void, decltype(&GDALClose)> ds(GDALOpenEx(m_gpkgPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr), GDALClose);
  if (!ds) { if (error) *error = QObject::tr("约束历史资产不可读取"); return {}; }
  return readHistory(ds.get(), "horizon", horizon, limit, offset, error, fidBelow, minFidOut);
}
bool ConstraintStore::copyAudit(const QString &destination, const QString &horizon, QString *error, int *copied) const {
  if (copied) *copied = 0;
  const auto sameFile = !QFileInfo(m_gpkgPath).canonicalFilePath().isEmpty() &&
      QFileInfo(m_gpkgPath).canonicalFilePath() == QFileInfo(destination).canonicalFilePath();
  if (sameFile || destination == m_gpkgPath) {
    if (error) *error = QObject::tr("约束审计副本必须写入新的快照文件");
    return false;
  }
  if (horizon.isEmpty() || !QFile::exists(m_gpkgPath)) return true;
  QVector<QVariantMap> events;
  // L-26：游标翻页（页内最小 fid 续扫）——OFFSET 对导出期间并发追加不稳
  // 定，会整页跳过事件；fid 单调 + event_id 去重兜底重复方向。
  for (qint64 fidBelow = -1;;) {
    QString reason; qint64 minFid = 0;
    const auto page = historyForHorizon(horizon, 200, 0, &reason, fidBelow, &minFid);
    if (!reason.isEmpty()) { if (error) *error = reason; return false; }
    events += page;
    // 判终只看「整页无有效行」：readHistory 会跳过损坏 JSON 行（页变短但
    // 后续页仍有数据），「页短即终页」会在损坏密集区静默截断快照；游标
    // 每轮严格递减（fid 单调 + fid<fidBelow），无死循环面。
    if (page.isEmpty() || minFid == 0) break;
    fidBelow = minFid;
  }
  if (events.isEmpty()) return true;
  Transaction transaction; QString reason;
  if (!transaction.open(destination, true, &reason) || !ensureAuditTable(transaction.ds)) {
    if (error) *error = reason.isEmpty() ? QObject::tr("无法在约束快照中建立审计表") : reason;
    return false;
  }
  auto audit = GDALDatasetGetLayerByName(transaction.ds, "constraint_audit");
  int written = 0;
  for (int i = events.size() - 1; i >= 0; --i) {
    const auto &event = events.at(i);
    const auto id = event.value("event_id").toString();
    if (id.isEmpty()) { if (error) *error = QObject::tr("约束审计事件缺少身份"); return false; }
    const auto existing = ("event_id=" + quoteSql(id)).toUtf8();
    if (OGR_L_SetAttributeFilter(audit, existing.constData()) != OGRERR_NONE) {
      if (error) *error = QObject::tr("约束快照审计表不可查询"); return false;
    }
    OGR_L_ResetReading(audit); Feature prior(OGR_L_GetNextFeature(audit), OGR_F_Destroy);
    OGR_L_SetAttributeFilter(audit, nullptr);
    if (prior) continue;
    Feature feature(OGR_F_Create(OGR_L_GetLayerDefn(audit)), OGR_F_Destroy);
    const auto write = [&](const char *name, const QString &value) {
      OGR_F_SetFieldString(feature.get(), OGR_F_GetFieldIndex(feature.get(), name), value.toUtf8().constData());
    };
    write("event_id", id);
    write("constraint_id", event.value("target_object_id").toString());
    write("horizon", event.value("horizon").toString());
    write("operation_id", event.value("operation_id").toString());
    write("actor_id", event.value("actor_id").toString());
    write("action", event.value("action").toString());
    write("timestamp", event.value("timestamp").toString());
    write("event_json", QString::fromUtf8(QJsonDocument::fromVariant(event).toJson(QJsonDocument::Compact)));
    OGR_F_SetFieldInteger64(feature.get(), OGR_F_GetFieldIndex(feature.get(), "revision"), event.value("after_revision").toLongLong());
    if (OGR_L_CreateFeature(audit, feature.get()) != OGRERR_NONE) {
      if (error) *error = QObject::tr("约束审计未能写入快照"); return false;
    }
    ++written;
  }
  if (!transaction.commit(&reason)) { if (error) *error = reason; return false; }
  if (copied) *copied = written;
  return true;
}
bool ConstraintStore::updateParameters(const QString &id, const QString &paramsJson, int schemaVersion,
                                       const QString &semanticType, QString *error) {
  if (m_readOnly) { if (error) *error = QObject::tr("已发布约束快照只读"); return false; }
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    QString reason; Transaction transaction; if (!transaction.open(m_gpkgPath, false, &reason)) return {false, reason};
    auto layer = GDALDatasetGetLayerByName(transaction.ds, "constraints");
    auto feature = byId(layer, id, &reason);
    if (!feature) return {false, reason.isEmpty() ? QObject::tr("约束身份不存在") : reason};
    const auto before = featureSnapshot(feature.get());
    auto requested = before; requested.insert("params_json", paramsJson); requested.insert("schema_version", schemaVersion);
    if (before.value("params_fingerprint").toString() == parameterFingerprint(requested) &&
        (semanticType.isEmpty() || before.value("type").toString() == semanticType)) return {transaction.commit(&reason), reason};
    feature.reset();
    if (!ensureField(layer, "params_json", OFTString) || !ensureField(layer, "schema_version", OFTInteger) ||
        !ensureAudit(transaction.ds, layer)) return {false, QObject::tr("无法扩展约束审计字段")};
    feature = byId(layer, id, &reason);
    if (!feature) return {false, QObject::tr("约束身份不可恢复")};
    OGR_F_SetFieldString(feature.get(), OGR_F_GetFieldIndex(feature.get(), "params_json"), paramsJson.toUtf8().constData());
    OGR_F_SetFieldInteger(feature.get(), OGR_F_GetFieldIndex(feature.get(), "schema_version"), schemaVersion);
    if (!semanticType.isEmpty()) OGR_F_SetFieldString(feature.get(), OGR_F_GetFieldIndex(feature.get(), "type"), semanticType.toUtf8().constData());
    if (OGR_L_SetFeature(layer, feature.get()) != OGRERR_NONE) return {false, QObject::tr("约束参数写入失败")};
    if (!journal(transaction.ds, layer, feature.get(), before, featureSnapshot(feature.get()), m_audit,
          m_audit.operationId.isEmpty() ? freshId() : m_audit.operationId, &reason) || !transaction.commit(&reason)) return {false, reason};
    return {true, {}};
  };
  const auto result = m_enqueue ? m_enqueue(write) : write(); if (!result.ok && error) *error = result.error; return result.ok;
}
bool ConstraintStore::remove(const QString &id, QString *error) {
  if (m_readOnly) { if (error) *error = QObject::tr("已发布约束快照只读"); return false; }
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    if (!QFile::exists(m_gpkgPath)) return {true, {}};
    QString reason; Transaction transaction; if (!transaction.open(m_gpkgPath, false, &reason)) return {false, reason};
    auto layer = GDALDatasetGetLayerByName(transaction.ds, "constraints"); auto feature = byId(layer, id, &reason);
    if (!reason.isEmpty()) return {false, reason};
    if (!feature) return {transaction.commit(&reason), reason};
    const auto before = featureSnapshot(feature.get()); const auto fid = OGR_F_GetFID(feature.get()); feature.reset();
    if (!ensureAudit(transaction.ds, layer)) return {false, QObject::tr("无法扩展约束审计字段")};
    if (OGR_L_DeleteFeature(layer, fid) != OGRERR_NONE) return {false, QObject::tr("删除约束失败")};
    if (!journal(transaction.ds, layer, nullptr, before, {}, m_audit,
          m_audit.operationId.isEmpty() ? freshId() : m_audit.operationId, &reason) || !transaction.commit(&reason)) return {false, reason};
    return {true, {}};
  };
  const auto result = m_enqueue ? m_enqueue(write) : write(); if (!result.ok && error) *error = result.error; return result.ok;
}

ConstraintStore::CrsStorageStatus ConstraintStore::inspectCrsStorage() const {
  ensureGdalRegistered();
  CrsStorageStatus status;
  status.fileExists = QFile::exists(m_gpkgPath);
  if (!status.fileExists) return status;
  std::unique_ptr<void, decltype(&GDALClose)> ds(
      GDALOpenEx(m_gpkgPath.toUtf8().constData(), GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr, nullptr, nullptr),
      GDALClose);
  if (!ds) return status;
  OGRLayerH layer = GDALDatasetGetLayerByName(ds.get(), "constraints");
  status.layerExists = layer != nullptr;
  if (!layer) return status;
  if (OGRSpatialReferenceH srs = OGR_L_GetSpatialRef(layer)) {
    char *wkt = nullptr;
    if (OSRExportToWkt(srs, &wkt) == OGRERR_NONE && wkt) {
      status.hasSrs = true;
      status.srsWkt = QString::fromUtf8(wkt);
    }
    CPLFree(wkt);
    if (status.hasSrs) {
      status.isStoreCrs = status.srsWkt == DataCatalog::localGridCrsWkt();
      if (!status.isStoreCrs) {
        OGRSpatialReferenceH store = newStoreSrs();
        status.isStoreCrs = store && OSRIsSame(srs, store) != 0;
        if (store) OSRDestroySpatialReference(store);
      }
    }
  }
  status.legacyNoCrs = !status.hasSrs;
  return status;
}

bool ConstraintStore::migrateLegacyCrs(QString *error, QString *backupPathOut) {
  if (m_readOnly) { if (error) *error = QObject::tr("已发布约束快照只读"); return false; }
  // 单写者纪律：有队列就进队列（与其余写路径互斥；独占窗口内完成
  // checkpoint → 事务 → 备份 → 迁移 → 校验，外部写者进不来）。
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    QString reason;
    const bool ok = migrateLegacyCrsLocked(&reason, backupPathOut);
    return {ok, reason};
  };
  const auto result = m_enqueue ? m_enqueue(write) : write();
  if (!result.ok && error) *error = result.error;
  return result.ok;
}

bool ConstraintStore::restoreFileFromBackup(const QString &gpkgPath,
                                            const QString &backupPath,
                                            QString *error) {
  const auto bad = [error](const QString &message) {
    if (error) *error = message;
    return false;
  };
  // 备份先验证后删除：备份缺失/不可读时绝不能先动现场（否则把唯一的现网
  // 文件也删了）。
  if (!QFile::exists(backupPath) || !QFileInfo(backupPath).isReadable())
    return bad(QObject::tr("严重：备份不存在或不可读，保持现场不动：%1").arg(backupPath));
  // -wal/-shm 是迁移连接留下的 WAL 脚手架：只换主文件的话，SQLite 下次打开
  // 对着「还原后的旧主文件」重放遗留帧（帧校验和链不校验与主库同一性），
  // 产出备份页+迁移页的嵌合库——三件一起还原。
  //
  // #588：次序的失败窗口。旧次序 {-wal, -shm, 主文件} 里主文件删除失败
  // （Windows 句柄未关即恒失败，见 constraintstore.h 的调用前提）时，-wal/-shm
  // 已被删掉而旧主文件仍在盘上：下次打开丢失 WAL 里已提交的帧。改为「先把主
  // 文件改名腾位 → 删伴生件 → 用备份落位 → 清腾位文件」，任一步失败都把主
  // 文件改名回去，现场始终落在「旧主文件 + 旧 WAL」或「备份 + 无 WAL」二者
  // 之一，不存在半途。
  QString aside;
  for (int attempt = 0; attempt < 64; ++attempt) {
    aside = gpkgPath + QStringLiteral(".restore-old-") +
            QString::number(QDateTime::currentMSecsSinceEpoch()) +
            (attempt == 0 ? QString() : QStringLiteral("-%1").arg(attempt));
    if (!QFile::exists(aside))
      break;
  }
  if (QFile::exists(aside))
    return bad(QObject::tr("严重：腾位文件名被占用，保持现场不动：%1").arg(aside));
  if (QFile::exists(gpkgPath) && !QFile::rename(gpkgPath, aside))
    return bad(QObject::tr("严重：无法腾出待还原文件 %1（文件被占用？）").arg(gpkgPath));
  // 现场回滚：把腾位的主文件改回原位。改不回去时如实报双重故障。
  const auto rollback = [&](const QString &message) {
    if (QFile::exists(aside) && !QFile::rename(aside, gpkgPath))
      return bad(QObject::tr("严重：%1；且无法把 %2 改回 %3，请人工恢复").arg(message, aside,
                                                                             gpkgPath));
    return bad(message);
  };
  for (const QString &suffix : {QStringLiteral("-wal"), QStringLiteral("-shm")}) {
    const QString target = gpkgPath + suffix;
    if (QFile::exists(target) && !QFile::remove(target))
      return rollback(QObject::tr("严重：无法移除待还原文件 %1（文件被占用？）").arg(target));
  }
  if (!QFile::copy(backupPath, gpkgPath) ||
      QFileInfo(backupPath).size() != QFileInfo(gpkgPath).size())
    return rollback(
        QObject::tr("严重：从备份还原失败，原文件可能缺失，备份在 %1").arg(backupPath));
  // 腾位文件：清不掉也只是个残留副本，不影响库的打开。
  QFile::remove(aside);
  return true;
}

bool ConstraintStore::migrateLegacyCrsLocked(QString *error, QString *backupPathOut) {
  const auto fail = [error](const QString &message) {
    if (error) *error = message;
    return false;
  };
  if (!QFile::exists(m_gpkgPath)) return fail(QObject::tr("约束库文件不存在，无 CRS 可迁移"));
  const CrsStorageStatus before = inspectCrsStorage();
  if (!before.layerExists) return fail(QObject::tr("约束库缺少 constraints 表，无 CRS 可迁移"));
  if (before.hasSrs) {
    if (!before.isStoreCrs)
      return fail(QObject::tr("约束库已登记其他 CRS 定义，拒绝覆盖：%1").arg(before.srsWkt));
    return true; // 已是目标状态：幂等成功，不产生新备份、不改文件。
  }
  // V3（复审）：WAL 盲区——备份前先把已提交数据 checkpoint 进主文件，字节级
  // 备份才完整；随后的 GDAL 事务持有写锁到提交，预扫描/备份/校验同窗生成，
  // 校验失败还原不会覆盖并发写者的合法提交。
  // V4（full-audit F4）：checkpoint 必须在事务外跑且 busy 标志必须为 0——
  // QGIS 画布等外部读者会令 TRUNCATE 检查点拿到 BUSY，此前返回值被丢弃，
  // 备份可能缺 WAL 中已提交的数据（还原即静默丢最近提交）。独占检查点拿
  // 不到时如实拒绝迁移，绝不带着不完整备份继续。
  {
    CPLErrorReset();
    GDALDatasetH ckptDs = GDALOpenEx(m_gpkgPath.toUtf8().constData(),
                                     GDAL_OF_UPDATE | GDAL_OF_VECTOR, nullptr,
                                     nullptr, nullptr);
    if (!ckptDs)
      return fail(QObject::tr("无法打开约束库做检查点：%1").arg(m_gpkgPath));
    bool ckptOk = false;
    OGRLayerH rows = GDALDatasetExecuteSQL(
        ckptDs, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, "SQLITE");
    if (rows) {
      if (OGRFeatureH raw = OGR_L_GetNextFeature(rows)) {
        ckptOk = OGR_F_GetFieldAsInteger(raw, 0) == 0; // busy==0 才完整
        OGR_F_Destroy(raw); // FR-01：feature 归还（本文件 Feature RAII 惯用法）
      }
      GDALDatasetReleaseResultSet(ckptDs, rows);
    }
    GDALClose(ckptDs);
    if (!ckptOk)
      return fail(QObject::tr("约束库被其他窗口占用（无法独占完成 WAL 检查点），"
                              "已中止迁移；请关闭其他打开该库的窗口后重试"));
  }
  Transaction transaction;
  if (!transaction.open(m_gpkgPath, false, error))
    return false;
  // 迁移前快照：fid/身份/层位/几何逐一记下，事务内读取。
  struct Row { qlonglong fid; QString id, horizon, wkt; };
  QVector<Row> pre;
  {
    OGRLayerH layer = GDALDatasetGetLayerByName(transaction.ds, "constraints");
    if (!layer) return fail(QObject::tr("约束库缺少 constraints 表"));
    OGR_L_ResetReading(layer);
    OGRWktOptions options(17, false);
    options.format = OGRWktFormat::G;
    while (OGRFeatureH raw = OGR_L_GetNextFeature(layer)) {
      Feature feature(raw, OGR_F_Destroy);
      const char *idStr = OGR_F_GetFieldAsString(feature.get(), OGR_F_GetFieldIndex(feature.get(), "id"));
      const char *hStr = OGR_F_GetFieldAsString(feature.get(), OGR_F_GetFieldIndex(feature.get(), "horizon"));
      OGRGeometryH geom = OGR_F_GetGeometryRef(feature.get());
      pre << Row{OGR_F_GetFID(feature.get()), QString::fromUtf8(idStr ? idStr : ""),
                 QString::fromUtf8(hStr ? hStr : ""),
                 geom ? QString::fromStdString(OGRGeometry::FromHandle(geom)->exportToWkt(options)) : QString()};
    }
  }
  // 字节级备份（checkpoint 后的主文件；校验失败时用它原样还原）。
  const QString backup = m_gpkgPath + QStringLiteral(".crs-backup-") +
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) +
                         QStringLiteral(".bak");
  if (QFile::exists(backup) && !QFile::remove(backup))
    return fail(QObject::tr("迁移备份文件已存在且无法替换：%1").arg(backup));
  if (!QFile::copy(m_gpkgPath, backup) || !QFile::exists(backup) ||
      QFileInfo(backup).size() != QFileInfo(m_gpkgPath).size())
    return fail(QObject::tr("无法创建迁移备份（或备份不完整）：%1").arg(backup));
  const auto restoreBackup = [&](QString *restoreError) {
    // 还原前必须已关闭本文件的全部连接（transaction.ds 在 commit 后、校验
    // ds 在调用点 reset）——Windows 上有句柄时 remove 直接失败；Linux 上
    // remove 虽成但句柄写回孤儿 inode。委托 restoreFileFromBackup 一并清
    // -wal/-shm（遗留 WAL 帧会重放到还原后的旧主文件上，产生嵌合库）。
    return restoreFileFromBackup(m_gpkgPath, backup, restoreError);
  };
  {
    const QByteArray wkt = DataCatalog::localGridCrsWkt().toUtf8();
    // 复用同定义条目（幂等重放不堆积 SRS 行）；否则分配 max(srs_id)+1。
    QString existing;
    {
      CPLErrorReset();
      auto rows = GDALDatasetExecuteSQL(transaction.ds,
          QStringLiteral("SELECT srs_id FROM gpkg_spatial_ref_sys WHERE definition=%1")
              .arg(quoteSql(QString::fromUtf8(wkt)))
              .toUtf8()
              .constData(),
          nullptr, "SQLITE");
      if (!rows) return fail(QObject::tr("迁移无法读取 gpkg_spatial_ref_sys：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg())));
      if (OGRFeatureH raw = OGR_L_GetNextFeature(rows)) {
        Feature f(raw, OGR_F_Destroy);
        existing = QString::number(OGR_F_GetFieldAsInteger64(f.get(), 0));
      }
      GDALDatasetReleaseResultSet(transaction.ds, rows);
    }
    QString srsId = existing;
    if (srsId.isEmpty()) {
      qlonglong next = 100000; // GPKG 保留 -1/0/4326 等；自定义条目从 100000 起避开
      CPLErrorReset();
      auto rows = GDALDatasetExecuteSQL(transaction.ds,
          "SELECT MAX(srs_id) FROM gpkg_spatial_ref_sys", nullptr, "SQLITE");
      if (rows) {
        if (OGRFeatureH raw = OGR_L_GetNextFeature(rows)) {
          Feature f(raw, OGR_F_Destroy);
          if (OGR_F_IsFieldSetAndNotNull(f.get(), 0))
            next = std::max<qlonglong>(next, OGR_F_GetFieldAsInteger64(f.get(), 0) + 1);
        }
        GDALDatasetReleaseResultSet(transaction.ds, rows);
      }
      srsId = QString::number(next);
      if (!sql(transaction.ds, QStringLiteral(
                   "INSERT INTO gpkg_spatial_ref_sys (srs_id, srs_name, organization, organization_coordsys_id, "
                   "definition, description) VALUES (%1, 'Paleo local engineering grid', 'NONE', %1, %2, "
                   "'paleo workstation constraint store CRS (explicit crs migration)')")
                   .arg(srsId, quoteSql(QString::fromUtf8(wkt)))
                   .toUtf8()))
        return fail(QObject::tr("迁移写入 SRS 定义失败：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg())));
    }
    if (!sql(transaction.ds, QStringLiteral("UPDATE gpkg_geometry_columns SET srs_id=%1 WHERE table_name='constraints'")
                 .arg(srsId)
                 .toUtf8()) ||
        !sql(transaction.ds, QStringLiteral("UPDATE gpkg_contents SET srs_id=%1 WHERE table_name='constraints'")
                 .arg(srsId)
                 .toUtf8()))
      return fail(QObject::tr("迁移登记 SRS 引用失败：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg())));
    // 可审计记录：迁移前后状态与备份位置进约束审计表（同事务，写入失败即迁移失败）。
    if (!ensureAuditTable(transaction.ds))
      return fail(QObject::tr("迁移无法建立约束审计表"));
    {
      const QVariantMap event{{"event_id", freshId()}, {"target_object_id", QString()},
          {"target_object_type", "constraint_store"}, {"horizon", QString()},
          {"operation_id", freshId()}, {"actor_id", localActor()}, {"identity_kind", "local_operator"},
          {"reason", QObject::tr("显式 CRS 迁移：无 SRS 历史库补登本地工程网格")},
          {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
          {"action", "crs_migration"}, {"change", "update"}, {"before_revision", 0}, {"after_revision", 0},
          {"backup", backup}, {"before", QVariantMap{{"has_srs", false}}},
          {"after", QVariantMap{{"has_srs", true}, {"srs_wkt", QString::fromUtf8(wkt)}}}};
      auto audit = GDALDatasetGetLayerByName(transaction.ds, "constraint_audit");
      Feature feature(OGR_F_Create(OGR_L_GetLayerDefn(audit)), OGR_F_Destroy);
      const auto write = [&](const char *name, const QString &value) {
        OGR_F_SetFieldString(feature.get(), OGR_F_GetFieldIndex(feature.get(), name), value.toUtf8().constData());
      };
      write("event_id", event.value("event_id").toString());
      write("constraint_id", QString()); write("horizon", QString());
      write("operation_id", event.value("operation_id").toString());
      write("actor_id", event.value("actor_id").toString()); write("action", QStringLiteral("crs_migration"));
      write("timestamp", event.value("timestamp").toString());
      write("event_json", QString::fromUtf8(QJsonDocument::fromVariant(event).toJson(QJsonDocument::Compact)));
      if (OGR_L_CreateFeature(audit, feature.get()) != OGRERR_NONE)
        return fail(QObject::tr("迁移审计事件写入失败，按可审计要求整单回滚"));
    }
    // 提交前校验（同一连接，写锁仍持有）：SRS 行已登记且引用已指向。
    {
      CPLErrorReset();
      auto rows = GDALDatasetExecuteSQL(transaction.ds,
          QStringLiteral("SELECT srs_id FROM gpkg_geometry_columns WHERE table_name='constraints'")
              .toUtf8()
              .constData(),
          nullptr, "SQLITE");
      bool verified = false;
      if (rows) {
        if (OGRFeatureH raw = OGR_L_GetNextFeature(rows)) {
          Feature f(raw, OGR_F_Destroy);
          verified = QString::number(OGR_F_GetFieldAsInteger64(f.get(), 0)) == srsId;
        }
        GDALDatasetReleaseResultSet(transaction.ds, rows);
      }
      if (!verified)
        return fail(QObject::tr("迁移提交前校验失败（SRS 引用未生效），已回滚"));
    }
    if (!transaction.commit(error))
      return false;
    // 提交即关闭事务连接：还原路径要在盘上换文件，句柄在身（Windows 必败；
    // Linux 则连接关闭时的 checkpoint 写进已 unlink 的孤儿 inode）。
    GDALClose(transaction.ds);
    transaction.ds = nullptr;
  }
  // 提交后独立重开校验：SRS 生效 + 每行 fid/身份/层位/几何逐一相等；失败时用
  // 同窗生成的备份原样还原（还原失败如实上报，绝不谎称已还原）。
  {
    const CrsStorageStatus after = inspectCrsStorage();
    if (!after.hasSrs || !after.isStoreCrs) {
      QString restoreError;
      const bool restored = restoreBackup(&restoreError);
      return fail(restored ? QObject::tr("迁移后校验失败（SRS 未生效），已从备份还原原文件")
                           : QObject::tr("迁移后校验失败。%1").arg(restoreError));
    }
    std::unique_ptr<void, decltype(&GDALClose)> ds(
        GDALOpenEx(m_gpkgPath.toUtf8().constData(), GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr, nullptr, nullptr),
        GDALClose);
    int index = 0;
    bool intact = ds != nullptr;
    if (intact) {
      OGRLayerH layer = GDALDatasetGetLayerByName(ds.get(), "constraints");
      OGR_L_ResetReading(layer);
      OGRWktOptions options(17, false);
      options.format = OGRWktFormat::G;
      while (OGRFeatureH raw = OGR_L_GetNextFeature(layer)) {
        Feature feature(raw, OGR_F_Destroy);
        if (index >= pre.size()) { intact = false; break; }
        const Row &expect = pre.at(index++);
        const char *idStr = OGR_F_GetFieldAsString(feature.get(), OGR_F_GetFieldIndex(feature.get(), "id"));
        const char *hStr = OGR_F_GetFieldAsString(feature.get(), OGR_F_GetFieldIndex(feature.get(), "horizon"));
        OGRGeometryH geom = OGR_F_GetGeometryRef(feature.get());
        const QString wkt = geom ? QString::fromStdString(OGRGeometry::FromHandle(geom)->exportToWkt(options)) : QString();
        if (OGR_F_GetFID(feature.get()) != expect.fid ||
            QString::fromUtf8(idStr ? idStr : "") != expect.id ||
            QString::fromUtf8(hStr ? hStr : "") != expect.horizon || wkt != expect.wkt) {
          intact = false;
          break;
        }
      }
    }
    if (!intact || index != pre.size()) {
      // 校验连接先关，再在盘上还原（restoreBackup 契约）。
      ds.reset();
      QString restoreError;
      const bool restored = restoreBackup(&restoreError);
      return fail(restored ? QObject::tr("迁移后校验失败（要素身份或几何变动），已从备份还原原文件")
                           : QObject::tr("迁移后校验失败。%1").arg(restoreError));
    }
  }
  if (backupPathOut) *backupPathOut = backup;
  return true;
}

bool ConstraintStore::replaceHorizon(const QString &horizon, const QVector<QVariantMap> &rows, QString *error) {
  if (m_readOnly) { if (error) *error = QObject::tr("已发布约束快照只读"); return false; }
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    QString reason; Transaction transaction;
    if (horizon.isEmpty() || !transaction.open(m_gpkgPath, false, &reason)) return {false, reason.isEmpty() ? QObject::tr("约束层位缺失") : reason};
    auto layer = GDALDatasetGetLayerByName(transaction.ds, "constraints");
    if (!layer) return {false, QObject::tr("约束图层缺失")};
    auto def = OGR_L_GetLayerDefn(layer); const auto hi = OGR_FD_GetFieldIndex(def, "horizon"), ii = OGR_FD_GetFieldIndex(def, "id");
    if (hi < 0 || ii < 0) return {false, QObject::tr("约束身份字段缺失")};
    struct Pending { qlonglong fid; QVariantMap before, after; QVariantMap input; bool exists; };
    QVector<Pending> pending; QSet<qlonglong> keep; QSet<QString> ids;
    for (const auto &row : rows) {
      const auto fid = row.value("fid", -1).toLongLong(); const auto id = row.value("id").toString();
      if (fid < 0 || id.isEmpty() || keep.contains(fid) || ids.contains(id) || row.value("horizon").toString() != horizon)
        return {false, QObject::tr("约束快照身份无效或重复")};
      keep.insert(fid); ids.insert(id);
      Feature feature(OGR_L_GetFeature(layer, fid), OGR_F_Destroy); const bool exists = bool(feature);
      const auto before = featureSnapshot(feature.get());
      if (exists && (before.value("id").toString() != id || before.value("horizon").toString() != horizon))
        return {false, QObject::tr("约束快照会覆盖其他要素身份")};
      if (!exists) {
        auto other = byId(layer, id, &reason); if (other || !reason.isEmpty()) return {false, QObject::tr("约束稳定身份已被其他要素使用")};
        feature.reset(OGR_F_Create(def)); OGR_F_SetFID(feature.get(), fid);
      }
      if (!assignGeometry(feature.get(), row.value("wkt").toString(), &reason)) return {false, reason};
      for (int i = 0; i < OGR_FD_GetFieldCount(def); ++i) {
        const auto field = OGR_FD_GetFieldDefn(def, i); const auto name = QString::fromUtf8(OGR_Fld_GetNameRef(field));
        // Replaying native snapshots cannot rewind append-only history.
        if (name == "evidence_revision" || name == "evidence_recent_events" || !row.contains(name)) continue;
        const auto value = row.value(name);
        if (value.isNull()) OGR_F_SetFieldNull(feature.get(), i);
        else switch (OGR_Fld_GetType(field)) {
          case OFTInteger: OGR_F_SetFieldInteger(feature.get(), i, value.toInt()); break;
          case OFTInteger64: OGR_F_SetFieldInteger64(feature.get(), i, value.toLongLong()); break;
          case OFTReal: if (!std::isfinite(value.toDouble())) return {false, QObject::tr("约束数值参数无效")};
                       OGR_F_SetFieldDouble(feature.get(), i, value.toDouble()); break;
          case OFTString: OGR_F_SetFieldString(feature.get(), i, value.toString().toUtf8().constData()); break;
          default: return {false, QObject::tr("约束快照字段类型不支持")};
        }
      }
      const auto after = featureSnapshot(feature.get());
      if (!sameBusinessState(before, after)) pending << Pending{fid, before, after, row, exists};
    }
    OGR_L_ResetReading(layer);
    while (auto raw = OGR_L_GetNextFeature(layer)) {
      Feature feature(raw, OGR_F_Destroy); const auto fid = OGR_F_GetFID(raw);
      if (QString::fromUtf8(OGR_F_GetFieldAsString(raw, hi)) == horizon && !keep.contains(fid))
        pending << Pending{fid, featureSnapshot(raw), {}, {}, true};
    }
    if (pending.isEmpty()) return {transaction.commit(&reason), reason};
    if (!ensureAudit(transaction.ds, layer)) return {false, QObject::tr("无法扩展约束审计字段")};
    def = OGR_L_GetLayerDefn(layer);
    const auto operation = m_audit.operationId.isEmpty() ? freshId() : m_audit.operationId;
    for (const auto &change : pending) {
      if (change.after.isEmpty()) {
        if (OGR_L_DeleteFeature(layer, change.fid) != OGRERR_NONE ||
            !journal(transaction.ds, layer, nullptr, change.before, {}, m_audit, operation, &reason))
          return {false, reason.isEmpty() ? QObject::tr("删除约束快照失败") : reason};
        continue;
      }
      Feature feature(change.exists ? OGR_L_GetFeature(layer, change.fid) : OGR_F_Create(def), OGR_F_Destroy);
      if (!feature) return {false, QObject::tr("约束快照目标不可用")};
      if (!change.exists) OGR_F_SetFID(feature.get(), change.fid);
      if (!assignGeometry(feature.get(), change.after.value("wkt").toString(), &reason)) return {false, reason};
      for (int i = 0; i < OGR_FD_GetFieldCount(def); ++i) {
        const auto field = OGR_FD_GetFieldDefn(def, i); const auto name = QString::fromUtf8(OGR_Fld_GetNameRef(field));
        if (name == "evidence_revision" || name == "evidence_recent_events" || !change.after.contains(name)) continue;
        const auto value = change.after.value(name);
        if (value.isNull()) OGR_F_SetFieldNull(feature.get(), i);
        else switch (OGR_Fld_GetType(field)) {
          case OFTInteger: OGR_F_SetFieldInteger(feature.get(), i, value.toInt()); break;
          case OFTInteger64: OGR_F_SetFieldInteger64(feature.get(), i, value.toLongLong()); break;
          case OFTReal: OGR_F_SetFieldDouble(feature.get(), i, value.toDouble()); break;
          case OFTString: OGR_F_SetFieldString(feature.get(), i, value.toString().toUtf8().constData()); break;
          default: return {false, QObject::tr("约束快照字段类型不支持")};
        }
      }
      if ((change.exists ? OGR_L_SetFeature(layer, feature.get()) : OGR_L_CreateFeature(layer, feature.get())) != OGRERR_NONE ||
          !journal(transaction.ds, layer, feature.get(), change.before, change.after, m_audit, operation, &reason))
        return {false, reason.isEmpty() ? QObject::tr("写入约束快照失败") : reason};
    }
    return {transaction.commit(&reason), reason};
  };
  const auto result = m_enqueue ? m_enqueue(write) : write(); if (!result.ok && error) *error = result.error; return result.ok;
}
