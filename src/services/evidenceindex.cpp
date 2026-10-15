// 层：数据
#include "evidenceindex.h"
#include "../catalog/datacatalog.h"
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariantList>
#include <algorithm>
#include <cpl_error.h>
#include <gdal.h>
#include <ogr_api.h>

namespace paleo::evidenceindex {
namespace {
constexpr int kSchemaVersion = 1;
constexpr int kMaxFeaturesPerVersion = 200000; // 病态文件读取上界
constexpr int kMaxQueryLimit = 1000;           // 查询 limit 钳制上界

// 与成果 GPKG 的字段契约（qgis/mappingfeatureidentity.h + 现有写入面）。
constexpr const char *kUidField = "paleo_feature_uid";
constexpr const char *kEvidenceField = "facies_evidence";
constexpr const char *kInterpretationField = "paleo_interpretation";
constexpr const char *kReviewField = "paleo_identity_review";

void note(QString *error, const QString &message) {
  if (error) *error = message;
}
bool fail(QString *error, const QString &message) {
  note(error, message);
  return false;
}

// 单条索引行的全部列（与 evidence_entries 列序一致）。
struct Row {
  QString versionId, assetId, layerId, title, horizon, featureUid,
      evidenceId, evidenceType, role, text, interpretationId, assertionId,
      reviewState, sourceVersion, sourceFeatureUid, actorId;
  bool identityReview = false, unlinked = false, sourceMissing = false,
       sourceFeatureMissing = false;
};

// ---- OGR 字段读取（缺列/空值 → 默认，不构成版本级失败）----
QString fieldString(OGRFeatureH feature, int index) {
  if (index < 0 || !OGR_F_IsFieldSetAndNotNull(feature, index)) return {};
  return QString::fromUtf8(OGR_F_GetFieldAsString(feature, index));
}
int fieldInt(OGRFeatureH feature, int index) {
  if (index < 0 || !OGR_F_IsFieldSetAndNotNull(feature, index)) return 0;
  return OGR_F_GetFieldAsInteger(feature, index);
}

// source_suffix（如 "|layername=facies_polygons"）里的图层名；无则空。
QString layerNameFromSuffix(const QString &suffix) {
  if (suffix.isEmpty()) return {};
  for (const auto &part : suffix.split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
    const auto token = part.trimmed();
    if (token.startsWith(QLatin1String("layername="), Qt::CaseInsensitive))
      return token.mid(QStringLiteral("layername=").size()).trimmed();
  }
  return {};
}

QJsonDocument parseJsonOrEmpty(const QString &raw, bool *ok) {
  *ok = true;
  const auto trimmed = raw.trimmed();
  if (trimmed.isEmpty()) return {};
  QJsonParseError parse;
  const auto doc = QJsonDocument::fromJson(trimmed.toUtf8(), &parse);
  if (parse.error != QJsonParseError::NoError) {
    *ok = false;
    return {};
  }
  return doc;
}

QString likePattern(const QString &text) {
  QString escaped;
  escaped.reserve(text.size() + 2);
  for (const QChar c : text) {
    if (c == QLatin1Char('%') || c == QLatin1Char('_') || c == QLatin1Char('\\'))
      escaped += QLatin1Char('\\');
    escaped += c;
  }
  return QLatin1Char('%') + escaped + QLatin1Char('%');
}

// 查询名 → WHERE 片段与绑定值个数（countFor 与各查询共用同一谓词口径）。
struct Predicate {
  QString where;
  int binds = 0;
};
Predicate predicateFor(const QString &name) {
  if (name == QLatin1String("featuresForEvidence"))
    return {QStringLiteral("evidence_id = ?"), 1};
  if (name == QLatin1String("featuresForInterpretation"))
    return {QStringLiteral("interpretation_id = ?"), 1};
  if (name == QLatin1String("featuresReferencingSource"))
    return {QStringLiteral("source_version = ?"), 1};
  if (name == QLatin1String("pendingReview"))
    return {QStringLiteral("horizon = ? AND (review_state IN ('NeedsReview','Proposed')"
                           " OR identity_review = 1)"), 1};
  if (name == QLatin1String("contradicted"))
    return {QStringLiteral("horizon = ? AND role LIKE '%CONTRADICTED%'"), 1};
  if (name == QLatin1String("staleSources"))
    return {QStringLiteral(
                "horizon = ? AND unlinked = 0 AND ("
                "(source_version = '' AND evidence_type IN"
                " ('well','seismic','single_factor','constraint'))"
                " OR source_missing = 1 OR source_feature_missing = 1)"), 1};
  if (name == QLatin1String("search"))
    return {QStringLiteral("(text LIKE ? ESCAPE '\\' OR actor_id LIKE ? ESCAPE '\\'"
                           " OR evidence_id LIKE ? ESCAPE '\\')"), 3};
  return {};
}

// 有界取行：limit 钳制 [1,1000]，offset 钳制非负，按 rowid 稳定排序。
QVector<Entry> fetchEntries(QSqlDatabase &db, const QString &where,
                            const QVariantList &binds, int limit, int offset,
                            QString *error) {
  QVector<Entry> out;
  QSqlQuery q(db);
  q.prepare(QStringLiteral(
                "SELECT version_id, asset_id, layer_id, title, horizon, feature_uid, "
                "evidence_id, evidence_type, role, text, interpretation_id, "
                "assertion_id, review_state, source_version, source_feature_uid, "
                "actor_id, identity_review, unlinked FROM evidence_entries "
                "WHERE %1 ORDER BY rowid LIMIT %2 OFFSET %3")
                .arg(where, QString::number(qBound(1, limit, kMaxQueryLimit)),
                     QString::number(qMax(0, offset))));
  for (const auto &value : binds) q.addBindValue(value);
  if (!q.exec()) {
    note(error, q.lastError().text());
    return out;
  }
  while (q.next()) {
    Entry e;
    e.versionId = q.value(0).toString();
    e.assetId = q.value(1).toString();
    e.layerId = q.value(2).toString();
    e.title = q.value(3).toString();
    e.horizon = q.value(4).toString();
    e.featureUid = q.value(5).toString();
    e.evidenceId = q.value(6).toString();
    e.evidenceType = q.value(7).toString();
    e.role = q.value(8).toString();
    e.text = q.value(9).toString();
    e.interpretationId = q.value(10).toString();
    e.assertionId = q.value(11).toString();
    e.reviewState = q.value(12).toString();
    e.sourceVersion = q.value(13).toString();
    e.sourceFeatureUid = q.value(14).toString();
    e.actorId = q.value(15).toString();
    e.identityReview = q.value(16).toInt() != 0;
    e.unlinked = q.value(17).toInt() != 0;
    out.append(e);
  }
  return out;
}

// 一个版本的一次完整替换（事务内 DELETE + INSERT + upsert indexed_versions）。
bool writeVersionRows(QSqlDatabase &db, const QString &versionId,
                      const QString &sha, int corruptRecords,
                      const QString &versionError, const QVector<Row> &rows,
                      QString *error) {
  if (!db.transaction())
    return fail(error, QStringLiteral("无法开始索引写入事务"));
  {
    QSqlQuery drop(db);
    drop.prepare(QStringLiteral("DELETE FROM evidence_entries WHERE version_id = ?"));
    drop.addBindValue(versionId);
    if (!drop.exec())
      return fail(error, QStringLiteral("无法清理旧索引条目：%1").arg(drop.lastError().text()));
    if (!rows.isEmpty()) {
      QSqlQuery insert(db);
      insert.prepare(QStringLiteral(
          "INSERT INTO evidence_entries (version_id, asset_id, layer_id, title, "
          "horizon, feature_uid, evidence_id, evidence_type, role, text, "
          "interpretation_id, assertion_id, review_state, source_version, "
          "source_feature_uid, actor_id, identity_review, unlinked, "
          "source_missing, source_feature_missing) "
          "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
      // NULL QString 一律绑定空串：NOT NULL 列不接受 SQL NULL，「空」与
      // 「未钉定」用空串表达（查询侧 isNull()/isEmpty() 同义于此语义）。
      const auto text = [](const QString &value) {
        return value.isNull() ? QStringLiteral("") : value;
      };
      for (const auto &row : rows) {
        insert.addBindValue(text(row.versionId));
        insert.addBindValue(text(row.assetId));
        insert.addBindValue(text(row.layerId));
        insert.addBindValue(text(row.title));
        insert.addBindValue(text(row.horizon));
        insert.addBindValue(text(row.featureUid));
        insert.addBindValue(text(row.evidenceId));
        insert.addBindValue(text(row.evidenceType));
        insert.addBindValue(text(row.role));
        insert.addBindValue(text(row.text));
        insert.addBindValue(text(row.interpretationId));
        insert.addBindValue(text(row.assertionId));
        insert.addBindValue(text(row.reviewState));
        insert.addBindValue(text(row.sourceVersion));
        insert.addBindValue(text(row.sourceFeatureUid));
        insert.addBindValue(text(row.actorId));
        insert.addBindValue(row.identityReview ? 1 : 0);
        insert.addBindValue(row.unlinked ? 1 : 0);
        insert.addBindValue(row.sourceMissing ? 1 : 0);
        insert.addBindValue(row.sourceFeatureMissing ? 1 : 0);
        if (!insert.exec())
          return fail(error, QStringLiteral("无法写入索引条目：%1").arg(insert.lastError().text()));
      }
    }
    QSqlQuery mark(db);
    mark.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO indexed_versions "
        "(version_id, sha256, indexed_at, corrupt_records, error) "
        "VALUES (?,?,?,?,?)"));
    mark.addBindValue(versionId);
    mark.addBindValue(sha);
    mark.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    mark.addBindValue(corruptRecords);
    mark.addBindValue(versionError);
    if (!mark.exec())
      return fail(error, QStringLiteral("无法登记版本索引状态：%1").arg(mark.lastError().text()));
  }
  if (!db.commit())
    return fail(error, QStringLiteral("无法提交索引写入事务"));
  return true;
}

// OGR 读取一个成果版本 → 索引行。文件/图层打开失败 → versionError 非空
//（记入 indexed_versions，不中断整体同步）；单条损坏 JSON 记录跳过并计数。
// cancelled 命中 → *aborted = true（调用方中止整轮同步）。
QVector<Row> readVersion(const CatalogVersion &version, const QString &filePath,
                         const QString &layerName, int *corruptRecords,
                         QString *versionError,
                         const std::function<bool()> &cancelled, bool *aborted) {
  *aborted = false;
  QVector<Row> rows;
  const auto extra = version.extra;
  const QString fallbackLayerId = QStringLiteral("product.%1").arg(version.id);
  const QString layerId = extra.value(QStringLiteral("layer_id")).toString().isEmpty()
                              ? fallbackLayerId
                              : extra.value(QStringLiteral("layer_id")).toString();
  const QString horizon = extra.value(QStringLiteral("horizon")).toString();
  const QString title = extra.value(QStringLiteral("title")).toString();
  CPLPushErrorHandler(CPLQuietErrorHandler); // 坏文件是数据事实，不让 GDAL 刷屏
  GDALDatasetH ds = GDALOpenEx(filePath.toUtf8().constData(),
                               GDAL_OF_READONLY | GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
  if (!ds) {
    CPLPopErrorHandler();
    *versionError = QStringLiteral("无法打开成果文件：%1").arg(filePath);
    return rows;
  }
  OGRLayerH layer = layerName.isEmpty() ? nullptr
                                        : GDALDatasetGetLayerByName(ds, layerName.toUtf8().constData());
  if (!layer) layer = GDALDatasetGetLayer(ds, 0); // 后备：首层
  if (!layer) {
    GDALClose(ds);
    CPLPopErrorHandler();
    *versionError = QStringLiteral("成果文件不含矢量图层：%1").arg(filePath);
    return rows;
  }
  OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
  const int uidIndex = OGR_FD_GetFieldIndex(defn, kUidField);
  const int evidenceIndex = OGR_FD_GetFieldIndex(defn, kEvidenceField);
  const int interpretationIndex = OGR_FD_GetFieldIndex(defn, kInterpretationField);
  const int reviewIndex = OGR_FD_GetFieldIndex(defn, kReviewField);
  OGR_L_ResetReading(layer);
  qint64 seen = 0;
  OGRFeatureH feature = nullptr;
  while ((feature = OGR_L_GetNextFeature(layer)) != nullptr) {
    const QString uid = fieldString(feature, uidIndex);
    const bool identityReview = fieldInt(feature, reviewIndex) != 0;
    bool evidenceOk = true;
    const auto evidenceDoc = parseJsonOrEmpty(fieldString(feature, evidenceIndex), &evidenceOk);
    if (!evidenceOk) ++(*corruptRecords); // 字段级损坏：如实计 1 条
    bool interpretationOk = true;
    const auto interpretationDoc =
        parseJsonOrEmpty(fieldString(feature, interpretationIndex), &interpretationOk);
    if (!interpretationOk) ++(*corruptRecords);
    OGR_F_Destroy(feature);
    ++seen;
    const auto interpretation =
        interpretationDoc.isArray() ? QJsonObject() : interpretationDoc.object();
    const QString interpretationId = interpretation.value(QStringLiteral("id")).toString();
    const QString assertionId = interpretation.value(QStringLiteral("assertion_id")).toString();
    const QString reviewState = interpretation.value(QStringLiteral("status")).toString();
    const QString interpretationHorizon = interpretation.value(QStringLiteral("horizon")).toString();
    const QString interpretationActor = interpretation.value(QStringLiteral("actor_id")).toString();
    QString rowHorizon = !horizon.isEmpty() ? horizon : interpretationHorizon;
    const auto records = evidenceDoc.array();
    if (records.isEmpty()) {
      // 无记录：仍有解释或待复核标记 → 一条图斑级行（evidence_id 为空）。
      if (interpretationId.isEmpty() && !identityReview) continue;
      Row row;
      row.versionId = version.id;
      row.assetId = version.assetId;
      row.layerId = layerId;
      row.title = title;
      row.horizon = rowHorizon;
      row.featureUid = uid;
      row.interpretationId = interpretationId;
      row.assertionId = assertionId;
      row.reviewState = reviewState;
      row.actorId = interpretationActor;
      row.identityReview = identityReview;
      rows.append(row);
      if (seen >= kMaxFeaturesPerVersion) break; // 有界读取
      if ((seen & 0xFFF) == 0 && cancelled && cancelled()) {
        *aborted = true;
        break;
      }
      continue;
    }
    for (const auto &item : records) {
      if (!item.isObject()) {
        ++(*corruptRecords); // 单条损坏记录：跳过并计数，不中断
        continue;
      }
      const auto record = item.toObject();
      QString evidenceId = record.value(QStringLiteral("id")).toString();
      if (evidenceId.isEmpty())
        evidenceId = record.value(QStringLiteral("evidence_id")).toString();
      if (evidenceId.isEmpty()) {
        ++(*corruptRecords); // 证据身份缺失：与域校验同判，不入检索
        continue;
      }
      Row row;
      row.versionId = version.id;
      row.assetId = version.assetId;
      row.layerId = layerId;
      row.title = title;
      row.horizon = rowHorizon.isEmpty()
                        ? record.value(QStringLiteral("horizon")).toString()
                        : rowHorizon;
      row.featureUid = uid;
      row.evidenceId = evidenceId;
      row.evidenceType = record.value(QStringLiteral("type")).toString();
      row.role = record.value(QStringLiteral("role")).toString();
      row.text = record.value(QStringLiteral("text")).toString();
      row.interpretationId = interpretationId;
      row.assertionId = assertionId;
      row.reviewState = reviewState;
      row.sourceVersion = record.value(QStringLiteral("source_version")).toString();
      row.sourceFeatureUid = record.value(QStringLiteral("source_feature_uid")).toString();
      row.actorId = record.value(QStringLiteral("actor_id")).toString();
      if (row.actorId.isEmpty()) row.actorId = interpretationActor;
      row.identityReview = identityReview;
      row.unlinked = record.value(QStringLiteral("unlinked"))
                         .toBool(record.value(QStringLiteral("tombstone")).toBool(false));
      row.sourceMissing = record.value(QStringLiteral("source_missing")).toBool(false);
      row.sourceFeatureMissing =
          record.value(QStringLiteral("source_feature_missing")).toBool(false);
      rows.append(row);
    }
    if (seen >= kMaxFeaturesPerVersion) break; // 有界读取
    if ((seen & 0xFFF) == 0 && cancelled && cancelled()) {
      *aborted = true;
      break;
    }
  }
  GDALClose(ds);
  CPLPopErrorHandler();
  return rows;
}
} // namespace

Index::~Index() { close(); }

bool Index::open(const QString &projectDir, QString *error) {
  if (m_open) close();
  if (projectDir.trimmed().isEmpty())
    return fail(error, QStringLiteral("工程目录为空，无法打开证据索引"));
  QDir dir(projectDir);
  if (!dir.exists() && !QDir::root().mkpath(dir.absolutePath()))
    return fail(error, QStringLiteral("无法创建工程目录：%1").arg(projectDir));
  if (!dir.mkpath(QStringLiteral("artifacts")))
    return fail(error, QStringLiteral("无法创建 artifacts 目录：%1").arg(projectDir));
  const QString path = dir.filePath(QStringLiteral("artifacts/evidence-index.sqlite"));
  m_connection = QStringLiteral("paleo_evidence_index_%1")
                     .arg(QString::number(reinterpret_cast<quintptr>(this), 16));
  {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(path);
    if (!db.open())
      return fail(error, QStringLiteral("无法打开证据索引文件 %1：%2")
                                 .arg(path, db.lastError().text()));
    const QStringList pragmas{
        QStringLiteral("PRAGMA busy_timeout = 5000"),
        QStringLiteral("PRAGMA journal_mode = WAL"),
        QStringLiteral("PRAGMA synchronous = FULL")};
    for (const auto &pragma : pragmas) {
      QSqlQuery q(db);
      if (!q.exec(pragma))
        return fail(error, QStringLiteral("索引 PRAGMA 失败：%1").arg(pragma));
    }
    const QStringList schema{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS evidence_entries ("
            " version_id TEXT NOT NULL,"
            " asset_id TEXT NOT NULL DEFAULT '',"
            " layer_id TEXT NOT NULL DEFAULT '',"
            " title TEXT NOT NULL DEFAULT '',"
            " horizon TEXT NOT NULL DEFAULT '',"
            " feature_uid TEXT NOT NULL DEFAULT '',"
            " evidence_id TEXT NOT NULL DEFAULT '',"
            " evidence_type TEXT NOT NULL DEFAULT '',"
            " role TEXT NOT NULL DEFAULT '',"
            " text TEXT NOT NULL DEFAULT '',"
            " interpretation_id TEXT NOT NULL DEFAULT '',"
            " assertion_id TEXT NOT NULL DEFAULT '',"
            " review_state TEXT NOT NULL DEFAULT '',"
            " source_version TEXT NOT NULL DEFAULT '',"
            " source_feature_uid TEXT NOT NULL DEFAULT '',"
            " actor_id TEXT NOT NULL DEFAULT '',"
            " identity_review INTEGER NOT NULL DEFAULT 0,"
            " unlinked INTEGER NOT NULL DEFAULT 0,"
            " source_missing INTEGER NOT NULL DEFAULT 0,"
            " source_feature_missing INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_entries_evidence ON evidence_entries(evidence_id)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_entries_source_version"
            " ON evidence_entries(source_version)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_entries_horizon_review"
            " ON evidence_entries(horizon, review_state)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_entries_horizon_role"
            " ON evidence_entries(horizon, role)"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_entries_feature ON evidence_entries(feature_uid)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS indexed_versions ("
            " version_id TEXT PRIMARY KEY NOT NULL,"
            " sha256 TEXT NOT NULL DEFAULT '',"
            " indexed_at TEXT NOT NULL DEFAULT '',"
            " corrupt_records INTEGER NOT NULL DEFAULT 0,"
            " error TEXT NOT NULL DEFAULT '')"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS index_meta ("
            " key TEXT PRIMARY KEY NOT NULL,"
            " value TEXT NOT NULL DEFAULT '')")};
    for (const auto &statement : schema) {
      QSqlQuery q(db);
      if (!q.exec(statement))
        return fail(error, QStringLiteral("索引建表失败：%1").arg(q.lastError().text()));
    }
    QSqlQuery version(db);
    if (!version.exec(QStringLiteral("PRAGMA user_version")) || !version.next())
      return fail(error, QStringLiteral("无法读取索引 schema 版本"));
    const int onDisk = version.value(0).toInt();
    if (onDisk > kSchemaVersion)
      return fail(error, QStringLiteral("证据索引 schema 版本过新（%1 > %2），请升级软件后重试")
                                 .arg(onDisk).arg(kSchemaVersion));
    if (onDisk < kSchemaVersion) {
      QSqlQuery set(db);
      if (!set.exec(QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion)))
        return fail(error, QStringLiteral("无法写入索引 schema 版本"));
    }
  }
  m_dir = projectDir;
  m_open = true;
  m_lastError.clear();
  return true;
}

void Index::close() {
  if (m_connection.isEmpty()) return;
  if (QSqlDatabase::contains(m_connection)) {
    {
      QSqlDatabase db = QSqlDatabase::database(m_connection, false);
      if (db.isValid()) db.close();
    }
    QSqlDatabase::removeDatabase(m_connection);
  }
  m_connection.clear();
  m_dir.clear();
  m_open = false;
}

bool Index::connection(QString *error) const {
  if (!m_open) return fail(error, QStringLiteral("证据索引未打开"));
  if (!QSqlDatabase::contains(m_connection))
    return fail(error, QStringLiteral("证据索引连接已失效"));
  return true;
}

int Index::synchronize(const DataCatalog *catalog, const QString &projectDir,
                       const std::function<bool()> &cancelled, QString *error) {
  m_lastError.clear();
  const auto abortWith = [&](const QString &message) -> int {
    m_lastError = message;
    if (error) *error = message;
    return -1;
  };
  if (!catalog) return abortWith(QStringLiteral("未提供数据目录"));
  QString connectError;
  if (!connection(&connectError)) return abortWith(connectError);
  QSqlDatabase db = QSqlDatabase::database(m_connection);

  // 候选：mapping_product 且 layer_type=vector 的全部版本（含 facies_polygons
  // 与其他矢量成果；raster 排除）。
  QHash<QString, CatalogVersion> wanted;
  for (const auto &v : catalog->versions()) {
    const auto e = v.extra;
    if (!e.value(QStringLiteral("mapping_product")).toBool() ||
        e.value(QStringLiteral("layer_type")).toString() != QLatin1String("vector"))
      continue;
    wanted.insert(v.id, v);
  }
  QHash<QString, QString> existing; // version_id → 已索引 sha
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT version_id, sha256 FROM indexed_versions")))
      return abortWith(q.lastError().text());
    while (q.next()) existing.insert(q.value(0).toString(), q.value(1).toString());
  }

  int changed = 0;
  for (auto it = wanted.cbegin(); it != wanted.cend(); ++it) {
    if (cancelled && cancelled())
      return abortWith(QStringLiteral("证据索引同步已取消"));
    const auto &v = it.value();
    const QString resolved = DataCatalog::resolvedVersionPath(projectDir, v);
    const QString filePath = resolved.section(QLatin1Char('|'), 0, 0);
    QString sha = v.sha256; // 受管副本校验和优先；外链/未留底则现算
    if (sha.isEmpty() && !filePath.isEmpty()) {
      QString shaError;
      sha = DataCatalog::sha256FileHex(filePath, &shaError, cancelled);
      // 文件缺失/不可读 → sha 为空：照常登记 error，下一轮重试（文件可能
      // 出现）；sha 为空永不参与「未变跳过」。
    }
    if (!sha.isEmpty() && sha == existing.value(v.id)) continue; // 未变，跳过
    int corrupt = 0;
    QString versionError;
    QVector<Row> rows;
    if (filePath.isEmpty()) {
      versionError = QStringLiteral("版本路径无法解析（不安全的受管路径或为空）");
    } else {
      bool aborted = false;
      rows = readVersion(v, filePath,
                         layerNameFromSuffix(
                             v.extra.value(QStringLiteral("source_suffix")).toString()),
                         &corrupt, &versionError, cancelled, &aborted);
      if (aborted) return abortWith(QStringLiteral("证据索引同步已取消"));
    }
    QString writeError;
    if (!writeVersionRows(db, v.id, sha, corrupt, versionError, rows, &writeError))
      return abortWith(writeError);
    ++changed;
  }

  // 目录里消失的版本：整版本条目删除（索引不是事实源，目录说了算）。
  for (auto it = existing.cbegin(); it != existing.cend(); ++it) {
    if (wanted.contains(it.key())) continue;
    if (!db.transaction())
      return abortWith(QStringLiteral("无法开始索引删除事务"));
    bool ok = true;
    {
      QSqlQuery drop(db);
      drop.prepare(QStringLiteral("DELETE FROM evidence_entries WHERE version_id = ?"));
      drop.addBindValue(it.key());
      ok = drop.exec();
      if (ok) {
        QSqlQuery mark(db);
        mark.prepare(QStringLiteral("DELETE FROM indexed_versions WHERE version_id = ?"));
        mark.addBindValue(it.key());
        ok = mark.exec();
      }
    }
    if (!ok || !db.commit()) {
      db.rollback();
      return abortWith(QStringLiteral("无法删除已消失版本的索引条目"));
    }
    ++changed;
  }

  QSqlQuery meta(db);
  meta.prepare(QStringLiteral(
      "INSERT OR REPLACE INTO index_meta (key, value) VALUES ('last_sync', ?)"));
  meta.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  if (!meta.exec()) return abortWith(meta.lastError().text());
  return changed;
}

QVector<Entry> Index::featuresForEvidence(const QString &evidenceId,
                                          int limit, int offset) const {
  m_lastError.clear();
  QString connectError;
  if (!connection(&connectError)) {
    m_lastError = connectError;
    return {};
  }
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  return fetchEntries(db, QStringLiteral("evidence_id = ?"), {evidenceId},
                      limit, offset, &m_lastError);
}

QVector<Entry> Index::featuresForInterpretation(const QString &interpretationId,
                                                int limit, int offset) const {
  m_lastError.clear();
  QString connectError;
  if (!connection(&connectError)) {
    m_lastError = connectError;
    return {};
  }
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  return fetchEntries(db, QStringLiteral("interpretation_id = ?"), {interpretationId},
                      limit, offset, &m_lastError);
}

QVector<Entry> Index::featuresReferencingSource(const QString &sourceVersionId,
                                                int limit, int offset) const {
  m_lastError.clear();
  QString connectError;
  if (!connection(&connectError)) {
    m_lastError = connectError;
    return {};
  }
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  return fetchEntries(db, QStringLiteral("source_version = ?"), {sourceVersionId},
                      limit, offset, &m_lastError);
}

QVector<Entry> Index::pendingReview(const QString &horizon,
                                    int limit, int offset) const {
  m_lastError.clear();
  QString connectError;
  if (!connection(&connectError)) {
    m_lastError = connectError;
    return {};
  }
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  return fetchEntries(db, predicateFor(QStringLiteral("pendingReview")).where, {horizon},
                      limit, offset, &m_lastError);
}

QVector<Entry> Index::contradicted(const QString &horizon,
                                   int limit, int offset) const {
  m_lastError.clear();
  QString connectError;
  if (!connection(&connectError)) {
    m_lastError = connectError;
    return {};
  }
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  return fetchEntries(db, predicateFor(QStringLiteral("contradicted")).where, {horizon},
                      limit, offset, &m_lastError);
}

QVector<Entry> Index::staleSources(const QString &horizon,
                                   int limit, int offset) const {
  m_lastError.clear();
  QString connectError;
  if (!connection(&connectError)) {
    m_lastError = connectError;
    return {};
  }
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  return fetchEntries(db, predicateFor(QStringLiteral("staleSources")).where, {horizon},
                      limit, offset, &m_lastError);
}

QVector<Entry> Index::search(const QString &text, int limit, int offset) const {
  m_lastError.clear();
  QString connectError;
  if (!connection(&connectError)) {
    m_lastError = connectError;
    return {};
  }
  if (text.trimmed().isEmpty()) return {}; // 空检索词不是全库匹配
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  const auto pattern = likePattern(text.trimmed());
  return fetchEntries(db, predicateFor(QStringLiteral("search")).where,
                      {pattern, pattern, pattern}, limit, offset, &m_lastError);
}

int Index::countFor(const QString &queryName, const QString &argument) const {
  m_lastError.clear();
  const auto predicate = predicateFor(queryName);
  if (predicate.where.isEmpty()) {
    m_lastError = QStringLiteral("未知计数查询：%1").arg(queryName);
    return -1;
  }
  QString connectError;
  if (!connection(&connectError)) {
    m_lastError = connectError;
    return -1;
  }
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  QSqlQuery q(db);
  q.prepare(QStringLiteral("SELECT COUNT(*) FROM evidence_entries WHERE %1")
                .arg(predicate.where));
  if (queryName == QLatin1String("search")) {
    const auto pattern = likePattern(argument.trimmed());
    q.addBindValue(pattern);
    q.addBindValue(pattern);
    q.addBindValue(pattern);
  } else {
    q.addBindValue(argument);
  }
  if (!q.exec() || !q.next()) {
    m_lastError = q.lastError().text();
    return -1;
  }
  return q.value(0).toInt();
}

QVariantMap Index::stats() const {
  QVariantMap out;
  m_lastError.clear();
  QString connectError;
  if (!connection(&connectError)) {
    m_lastError = connectError;
    return out;
  }
  QSqlDatabase db = QSqlDatabase::database(m_connection);
  QSqlQuery q(db);
  if (!q.exec(QStringLiteral(
          "SELECT (SELECT COUNT(*) FROM indexed_versions),"
          " (SELECT COUNT(*) FROM evidence_entries),"
          " (SELECT COALESCE(SUM(corrupt_records), 0) FROM indexed_versions),"
          " (SELECT COUNT(*) FROM indexed_versions WHERE error != ''),"
          " (SELECT value FROM index_meta WHERE key = 'last_sync')"))) {
    m_lastError = q.lastError().text();
    return out;
  }
  if (!q.next()) {
    m_lastError = QStringLiteral("无法读取索引统计");
    return out;
  }
  out.insert(QStringLiteral("versions_indexed"), q.value(0).toInt());
  out.insert(QStringLiteral("entries"), q.value(1).toInt());
  out.insert(QStringLiteral("corrupt_records"), q.value(2).toInt());
  out.insert(QStringLiteral("errored_versions"), q.value(3).toInt());
  out.insert(QStringLiteral("last_sync"), q.value(4).toString());
  return out;
}
} // namespace paleo::evidenceindex
