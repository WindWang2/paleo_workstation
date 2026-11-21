// 层：功能
#include "mapediteventhistory.h"
#include "mappingworkbench.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace paleo::mapedit {
namespace {
constexpr int kMaxVersions = 10000;
constexpr int kMaxScanned = 200000;

QString parentVersionId(const QVariantMap &extra, QStringList *diagnostics) {
  const auto link = extra.value(QStringLiteral("spatial_edit_history")).toMap();
  const auto linked = link.value(QStringLiteral("parent_version_id")).toString();
  if (!linked.isEmpty()) return linked;
  const auto parents = extra.value(QStringLiteral("computational_parent_versions"));
  QStringList ids;
  if (parents.typeId() == QMetaType::QStringList) ids = parents.toStringList();
  else for (const auto &entry : parents.toList()) ids << entry.toString();
  ids.removeAll(QString());
  if (ids.size() == 1) return ids.first();
  if (ids.size() > 1 && diagnostics)
    diagnostics->append(QStringLiteral("计算父版本不止一个，不能当作编辑链"));
  return {};
}

bool eventMatches(const QVariantMap &event, const QString &featureUid) {
  if (featureUid.isEmpty()) return true;
  return event.value(QStringLiteral("before_uid")).toString() == featureUid ||
         event.value(QStringLiteral("after_uid")).toString() == featureUid ||
         event.value(QStringLiteral("target_object_id")).toString() == featureUid ||
         event.value(QStringLiteral("feature_uid")).toString() == featureUid;
}

QString payloadStamp(const QVariantMap &event) {
  return event.value(QStringLiteral("before_evidence")).toString() + QChar(0x1f) +
         event.value(QStringLiteral("before_geometry")).toString() + QChar(0x1f) +
         event.value(QStringLiteral("after_fingerprint")).toString() + QChar(0x1f) +
         event.value(QStringLiteral("before_fingerprint")).toString();
}
}

QVariantMap spatialEditHistoryLink(const CatalogVersion &previous) {
  const auto prior = previous.extra.value(QStringLiteral("spatial_edit_history")).toMap();
  const bool parentDelta = prior.value(QStringLiteral("schema")).toString() ==
                           QLatin1String(kSpatialHistorySchema);
  return {{QStringLiteral("schema"), QString::fromLatin1(kSpatialHistorySchema)},
          {QStringLiteral("parent_version_id"), previous.id},
          {QStringLiteral("parent_revision"), previous.versionNumber},
          {QStringLiteral("parent_encoding"), parentDelta ? QStringLiteral("delta")
                                                          : QStringLiteral("cumulative")}};
}

QString spatialEventKey(const QVariantMap &event, QString *diagnostic) {
  const auto id = event.value(QStringLiteral("event_id")).toString();
  if (!id.isEmpty()) return QStringLiteral("id:") + id;
  const QStringList parts{event.value(QStringLiteral("operation_id")).toString(),
                          event.value(QStringLiteral("target_object_id")).toString(),
                          event.value(QStringLiteral("action")).toString(),
                          event.value(QStringLiteral("before_version_id")).toString(),
                          event.value(QStringLiteral("timestamp")).toString(),
                          event.value(QStringLiteral("before_uid")).toString(),
                          event.value(QStringLiteral("after_uid")).toString(),
                          event.value(QStringLiteral("before_revision")).toString(),
                          event.value(QStringLiteral("before_fingerprint")).toString(),
                          event.value(QStringLiteral("after_fingerprint")).toString()};
  int filled = 0;
  for (const auto &part : parts)
    if (!part.isEmpty()) ++filled;
  if (filled < 2) {
    if (diagnostic)
      *diagnostic = QStringLiteral("旧事件缺少稳定标识，已原样保留且不与其他事件合并");
    return {};
  }
  return QStringLiteral("legacy:") + parts.join(QChar(0x1f));
}

QVariantMap readSpatialEditHistory(const std::function<CatalogVersion(const QString &)> &lookup,
                                   const SpatialEditHistoryQuery &query) {
  QVariantMap out{{QStringLiteral("source_version_id"), query.versionId},
                  {QStringLiteral("events"), QVariantList{}},
                  {QStringLiteral("complete"), false},
                  {QStringLiteral("diagnostics"), QStringList{}},
                  {QStringLiteral("visited_versions"), 0},
                  {QStringLiteral("scanned_events"), 0},
                  {QStringLiteral("next_cursor"), QString{}}};
  auto diagnostics = QStringList{};
  const auto fail = [&](const QString &message) {
    diagnostics.append(message);
    out.insert(QStringLiteral("diagnostics"), diagnostics);
    out.insert(QStringLiteral("complete"), false);
    return out;
  };
  if (!lookup || query.versionId.isEmpty()) return fail(QStringLiteral("没有可读取的图件版本"));
  const int pageSize = qBound(1, query.pageSize <= 0 ? 50 : query.pageSize, kSpatialHistoryPageMax);
  QString versionId = query.versionId;
  int index = -1;
  if (!query.cursor.isEmpty()) {
    const auto parts = query.cursor.split(QLatin1Char('|'));
    if (parts.size() != 3 || parts.at(0) != QLatin1String("v1") || parts.at(1).isEmpty())
      return fail(QStringLiteral("历史游标无法识别"));
    versionId = parts.at(1);
    if (parts.at(2) == QLatin1String("end")) index = -1;
    else {
      bool ok = false;
      index = parts.at(2).toInt(&ok);
      if (!ok || index < 0) return fail(QStringLiteral("历史游标无法识别"));
    }
  }
  QVariantList page;
  QSet<QString> seen;
  QSet<QString> visited;
  int scanned = 0;
  bool stop = false;
  QString nextCursor;
  while (!stop && page.size() < pageSize) {
    if (visited.contains(versionId)) return fail(QStringLiteral("编辑历史出现循环"));
    if (visited.size() >= kMaxVersions || scanned >= kMaxScanned) {
      diagnostics.append(QStringLiteral("历史读取达到安全上限，结果不完整"));
      break;
    }
    visited.insert(versionId);
    const auto version = lookup(versionId);
    if (version.id.isEmpty()) {
      diagnostics.append(QStringLiteral("编辑历史缺少父版本：%1").arg(versionId));
      break;
    }
    const auto raw = version.extra.value(QStringLiteral("spatial_edit_events"));
    if (raw.isValid() && raw.typeId() != QMetaType::QVariantList) {
      diagnostics.append(QStringLiteral("空间编辑事件字段损坏"));
      break;
    }
    const auto events = raw.toList();
    if (index < 0) index = events.size();
    if (index > events.size()) return fail(QStringLiteral("历史游标超出该版本事件范围"));
    const bool delta = version.extra.value(QStringLiteral("spatial_edit_history")).toMap()
                           .value(QStringLiteral("schema")).toString() == QLatin1String(kSpatialHistorySchema);
    while (index > 0 && page.size() < pageSize) {
      if (scanned >= kMaxScanned) {
        diagnostics.append(QStringLiteral("历史读取达到安全上限，结果不完整"));
        stop = true;
        break;
      }
      --index;
      ++scanned;
      const auto event = events.at(index).toMap();
      if (!eventMatches(event, query.featureUid)) continue;
      QString keyDiagnostic;
      const auto key = spatialEventKey(event, &keyDiagnostic);
      if (!keyDiagnostic.isEmpty() && !diagnostics.contains(keyDiagnostic)) diagnostics.append(keyDiagnostic);
      if (key.isEmpty()) {
        page.prepend(event);
        continue;
      }
      const auto payloadKey = key + QChar(0x1e) + payloadStamp(event);
      if (seen.contains(key)) {
        if (!seen.contains(payloadKey)) {
          diagnostics.append(QStringLiteral("相同标识的空间事件内容不一致，未合并"));
          seen.insert(payloadKey);
          page.prepend(event);
        }
        continue;
      }
      seen.insert(key);
      seen.insert(payloadKey);
      page.prepend(event);
    }
    if (stop) break;
    if (page.size() >= pageSize) {
      if (index > 0) nextCursor = QStringLiteral("v1|%1|%2").arg(versionId).arg(index);
      else if (delta) {
        const auto parent = parentVersionId(version.extra, &diagnostics);
        if (!parent.isEmpty()) nextCursor = QStringLiteral("v1|%1|end").arg(parent);
      }
      break;
    }
    if (!delta) break;
    const auto parent = parentVersionId(version.extra, &diagnostics);
    if (parent.isEmpty()) {
      diagnostics.append(QStringLiteral("增量历史没有父版本"));
      break;
    }
    versionId = parent;
    index = -1;
  }
  out.insert(QStringLiteral("events"), page);
  out.insert(QStringLiteral("next_cursor"), nextCursor);
  out.insert(QStringLiteral("diagnostics"), diagnostics);
  out.insert(QStringLiteral("complete"), diagnostics.isEmpty() && nextCursor.isEmpty());
  out.insert(QStringLiteral("visited_versions"), visited.size());
  out.insert(QStringLiteral("scanned_events"), scanned);
  return out;
}
}

QVariantMap MappingWorkbench::spatialEditHistory(const QString &versionId, const QString &featureUid,
                                                 int pageSize, const QString &cursor) const {
  using paleo::mapedit::SpatialEditHistoryQuery;
  if (!m_catalog || !m_catalog->isOpen()) {
    return {{QStringLiteral("source_version_id"), versionId},
            {QStringLiteral("events"), QVariantList{}},
            {QStringLiteral("complete"), false},
            {QStringLiteral("diagnostics"), QStringList{tr("资料目录不可用，不能读取图件空间编辑历史")}},
            {QStringLiteral("next_cursor"), QString{}}};
  }
  SpatialEditHistoryQuery query;
  query.versionId = versionId;
  query.featureUid = featureUid;
  query.pageSize = pageSize;
  query.cursor = cursor;
  return paleo::mapedit::readSpatialEditHistory(
      [this](const QString &id) { return m_catalog->versionById(id); }, query);
}
