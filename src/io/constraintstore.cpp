// 层：数据
#include "constraintstore.h"
#include "gdalreg_internal.h"
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_geometry.h>
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
QVector<QVariantMap> readHistory(GDALDatasetH ds, const QString &column, const QString &key,
                                 int limit, int offset, QString *error) {
  QVector<QVariantMap> result;
  if (!GDALDatasetGetLayerByName(ds, "constraint_audit")) return result;
  // #441：列名白名单固定两条语句；键用 quoteSql 字面量，limit/offset 用十进制数字拼接。
  // 禁止对已拼好的 SQL 再调 QString::arg，否则键里的 %1/%3 会污染 LIMIT/OFFSET。
  QString query;
  if (column == QLatin1String("constraint_id")) {
    query = QStringLiteral("SELECT event_json FROM constraint_audit WHERE constraint_id=")
        + quoteSql(key)
        + QStringLiteral(" ORDER BY fid DESC LIMIT ")
        + QString::number(qBound(1, limit, 200))
        + QStringLiteral(" OFFSET ")
        + QString::number(qMax(0, offset));
  } else if (column == QLatin1String("horizon")) {
    query = QStringLiteral("SELECT event_json FROM constraint_audit WHERE horizon=")
        + quoteSql(key)
        + QStringLiteral(" ORDER BY fid DESC LIMIT ")
        + QString::number(qBound(1, limit, 200))
        + QStringLiteral(" OFFSET ")
        + QString::number(qMax(0, offset));
  } else {
    if (error) *error = QObject::tr("约束审计查询列无效");
    return result;
  }
  const auto sql = query.toUtf8();
  CPLErrorReset(); auto rows = GDALDatasetExecuteSQL(ds, sql.constData(), nullptr, "SQLITE");
  if (!rows) { if (error) *error = QString::fromUtf8(CPLGetLastErrorMsg()); return result; }
  while (auto raw = OGR_L_GetNextFeature(rows)) {
    Feature feature(raw, OGR_F_Destroy); QJsonParseError parse;
    auto document = QJsonDocument::fromJson(QByteArray(OGR_F_GetFieldAsString(raw, 0)), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
      if (error) *error = QObject::tr("约束审计记录损坏，不能作为已验证历史");
      result.clear(); break;
    }
    result << document.toVariant().toMap();
  }
  GDALDatasetReleaseResultSet(ds, rows); return result;
}
bool ensureAuditTable(GDALDatasetH ds) {
  auto audit = GDALDatasetGetLayerByName(ds, "constraint_audit");
  if (!audit) audit = GDALDatasetCreateLayer(ds, "constraint_audit", nullptr, wkbNone, nullptr);
  if (!audit) return false;
  for (const auto name : {"event_id", "constraint_id", "horizon", "operation_id", "actor_id", "action", "timestamp", "event_json"})
    if (!ensureField(audit, name, OFTString)) return false;
  return ensureField(audit, "revision", OFTInteger64) &&
      sql(ds, "CREATE UNIQUE INDEX IF NOT EXISTS paleo_constraint_event_id ON constraint_audit(event_id)") &&
      sql(ds, "CREATE INDEX IF NOT EXISTS paleo_constraint_history_id ON constraint_audit(constraint_id,fid)") &&
      sql(ds, "CREATE INDEX IF NOT EXISTS paleo_constraint_history_horizon ON constraint_audit(horizon,fid)");
}
bool ensureAudit(GDALDatasetH ds, OGRLayerH constraints) {
  return ensureAuditTable(ds) && constraints &&
      ensureField(constraints, "evidence_revision", OFTInteger64) &&
      ensureField(constraints, "evidence_recent_events", OFTString) &&
      sql(ds, "CREATE INDEX IF NOT EXISTS paleo_constraint_stable_id ON constraints(id)") &&
      sql(ds, "CREATE INDEX IF NOT EXISTS paleo_constraint_horizon ON constraints(horizon)");
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
    if (QFile::exists(file)) ds = GDALOpenEx(file.toUtf8().constData(), GDAL_OF_UPDATE | GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    else if (allowCreate && QDir().mkpath(QFileInfo(file).absolutePath())) {
      const auto driver = GDALGetDriverByName("GPKG");
      if (driver) ds = GDALCreate(driver, file.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr);
      created = ds != nullptr;
    }
    if (!ds || GDALDatasetStartTransaction(ds, FALSE) != OGRERR_NONE) {
      *error = QObject::tr("无法打开约束事务：%1").arg(QString::fromUtf8(CPLGetLastErrorMsg())); return false;
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
    const QString &type, int code, QString *error, double weight) {
  return appendInternal(horizon, id, wkt, type, code, nullptr, 0, error, weight);
}
bool ConstraintStore::appendExtended(const QString &horizon, const QString &id, const QString &wkt,
    const QString &type, int code, const QString &parameters, int schema, QString *error, double weight) {
  return appendInternal(horizon, id, wkt, type, code, &parameters, schema, error, weight);
}
bool ConstraintStore::ensureSchema(QString *error) {
  if (m_readOnly) { if (error) *error = QObject::tr("已发布约束快照只读"); return false; }
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    QString reason;
    Transaction transaction; if (!transaction.open(m_gpkgPath, true, &reason)) return {false, reason};
    auto layer = GDALDatasetGetLayerByName(transaction.ds, "constraints");
    if (!layer) layer = GDALDatasetCreateLayer(transaction.ds, "constraints", nullptr, wkbUnknown, nullptr);
    if (!layer) return {false, QObject::tr("无法建立约束图层")};
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
    const QString &type, int code, const QString *parameters, int schema, QString *error, double weight) {
  if (m_readOnly) { if (error) *error = QObject::tr("已发布约束快照只读"); return false; }
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    QString reason;
    if (horizon.trimmed().isEmpty() || id.trimmed().isEmpty() || type.trimmed().isEmpty() || !std::isfinite(weight))
      return {false, QObject::tr("约束身份、层位、语义或权重无效")};
    Transaction transaction; if (!transaction.open(m_gpkgPath, true, &reason)) return {false, reason};
    auto layer = GDALDatasetGetLayerByName(transaction.ds, "constraints");
    if (!layer) layer = GDALDatasetCreateLayer(transaction.ds, "constraints", nullptr, wkbUnknown, nullptr);
    if (!layer) return {false, QObject::tr("无法建立约束图层")};
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
    OGR_F_SetFieldInteger(feature.get(), OGR_F_GetFieldIndex(feature.get(), "facies_code"), code);
    OGR_F_SetFieldDouble(feature.get(), OGR_F_GetFieldIndex(feature.get(), "weight"), weight);
    if (parameters) { set("params_json", *parameters);
      OGR_F_SetFieldInteger(feature.get(), OGR_F_GetFieldIndex(feature.get(), "schema_version"), schema); }
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
QVector<QVariantMap> ConstraintStore::historyForHorizon(const QString &horizon, int limit, int offset, QString *error) const {
  ensureGdalRegistered(); if (!QFile::exists(m_gpkgPath)) return {};
  std::unique_ptr<void, decltype(&GDALClose)> ds(GDALOpenEx(m_gpkgPath.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr), GDALClose);
  if (!ds) { if (error) *error = QObject::tr("约束历史资产不可读取"); return {}; }
  return readHistory(ds.get(), "horizon", horizon, limit, offset, error);
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
  for (int offset = 0;; offset += 200) {
    QString reason; const auto page = historyForHorizon(horizon, 200, offset, &reason);
    if (!reason.isEmpty()) { if (error) *error = reason; return false; }
    events += page;
    if (page.size() < 200) break;
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
bool ConstraintStore::updateParameters(const QString &id, const QString &parameters, int schema,
                                       const QString &semantic, QString *error) {
  if (m_readOnly) { if (error) *error = QObject::tr("已发布约束快照只读"); return false; }
  const WriteFn write = [&]() -> PaleoProjectStore::WriteResult {
    QString reason; Transaction transaction; if (!transaction.open(m_gpkgPath, false, &reason)) return {false, reason};
    auto layer = GDALDatasetGetLayerByName(transaction.ds, "constraints");
    auto feature = byId(layer, id, &reason);
    if (!feature) return {false, reason.isEmpty() ? QObject::tr("约束身份不存在") : reason};
    const auto before = featureSnapshot(feature.get());
    auto requested = before; requested.insert("params_json", parameters); requested.insert("schema_version", schema);
    if (before.value("params_fingerprint").toString() == parameterFingerprint(requested) &&
        (semantic.isEmpty() || before.value("type").toString() == semantic)) return {transaction.commit(&reason), reason};
    feature.reset();
    if (!ensureField(layer, "params_json", OFTString) || !ensureField(layer, "schema_version", OFTInteger) ||
        !ensureAudit(transaction.ds, layer)) return {false, QObject::tr("无法扩展约束审计字段")};
    feature = byId(layer, id, &reason);
    if (!feature) return {false, QObject::tr("约束身份不可恢复")};
    OGR_F_SetFieldString(feature.get(), OGR_F_GetFieldIndex(feature.get(), "params_json"), parameters.toUtf8().constData());
    OGR_F_SetFieldInteger(feature.get(), OGR_F_GetFieldIndex(feature.get(), "schema_version"), schema);
    if (!semantic.isEmpty()) OGR_F_SetFieldString(feature.get(), OGR_F_GetFieldIndex(feature.get(), "type"), semantic.toUtf8().constData());
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
