// 层：视图
#include "wellcompositestore.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSettings>

namespace WellComposite
{

// ----------------------------------------------------------------------------
// 导出预设 / 对比模板 序列化
// ----------------------------------------------------------------------------
QVariantMap ExportPreset::toVariantMap() const
{
  QVariantMap m;
  m.insert(QStringLiteral("name"), name);
  m.insert(QStringLiteral("format"), format);
  m.insert(QStringLiteral("scale"), scaleRatio);
  m.insert(QStringLiteral("depthMode"), depthMode);
  m.insert(QStringLiteral("rangeTop"), rangeTop);
  m.insert(QStringLiteral("rangeBottom"), rangeBottom);
  m.insert(QStringLiteral("dpi"), dpi);
  m.insert(QStringLiteral("legend"), includeLegend);
  m.insert(QStringLiteral("header"), includeHeader);
  return m;
}

ExportPreset ExportPreset::fromVariantMap(const QVariantMap &map)
{
  ExportPreset p;
  p.name = map.value(QStringLiteral("name")).toString();
  p.format = map.value(QStringLiteral("format"), QStringLiteral("pdf")).toString();
  p.scaleRatio = map.value(QStringLiteral("scale"), QStringLiteral("1:500")).toString();
  p.depthMode = map.value(QStringLiteral("depthMode"), QStringLiteral("whole")).toString();
  p.rangeTop = map.value(QStringLiteral("rangeTop")).toDouble();
  p.rangeBottom = map.value(QStringLiteral("rangeBottom")).toDouble();
  p.dpi = map.value(QStringLiteral("dpi"), 300).toInt();
  p.includeLegend = map.value(QStringLiteral("legend"), true).toBool();
  p.includeHeader = map.value(QStringLiteral("header"), true).toBool();
  return p;
}

QVariantMap ComparisonTemplate::toVariantMap() const
{
  QVariantMap m;
  m.insert(QStringLiteral("name"), name);
  m.insert(QStringLiteral("wells"), wells);
  QVariantList trackList;
  for (const auto &t : tracks)
    trackList << t.toVariantMap();
  m.insert(QStringLiteral("tracks"), trackList);
  m.insert(QStringLiteral("linkScroll"), linkScroll);
  return m;
}

ComparisonTemplate ComparisonTemplate::fromVariantMap(const QVariantMap &map)
{
  ComparisonTemplate t;
  t.name = map.value(QStringLiteral("name")).toString();
  t.wells = map.value(QStringLiteral("wells")).toStringList();
  const QVariantList trackList = map.value(QStringLiteral("tracks")).toList();
  for (const auto &v : trackList)
    t.tracks << TrackSpec::fromVariantMap(v.toMap());
  t.linkScroll = map.value(QStringLiteral("linkScroll"), true).toBool();
  return t;
}

// ----------------------------------------------------------------------------
// 会话级（QSettings）
// ----------------------------------------------------------------------------
QString WellCompositeStore::sessionKey(const QString &project, const QString &well)
{
  const auto clean = [](const QString &s) {
    QString out = s.trimmed();
    if (out.isEmpty())
      out = QStringLiteral("_");
    return out.replace(QLatin1Char('/'), QLatin1Char('_'));
  };
  return QStringLiteral("wellcomposite/%1/%2").arg(clean(project), clean(well));
}

QList<TrackSpec> WellCompositeStore::specsFromJson(const QJsonArray &arr)
{
  QList<TrackSpec> specs;
  for (const auto &v : arr)
  {
    const QJsonObject o = v.toObject();
    TrackSpec spec;
    spec.typeId = o.value(QStringLiteral("type")).toString();
    spec.title = o.value(QStringLiteral("title")).toString();
    spec.width = o.value(QStringLiteral("width")).toDouble(100.0);
    spec.visible = o.value(QStringLiteral("visible")).toBool(true);
    spec.printIncluded = o.value(QStringLiteral("print")).toBool(true);
    spec.params = o.value(QStringLiteral("params")).toObject().toVariantMap();
    specs << spec;
  }
  return specs;
}

QJsonArray WellCompositeStore::specsToJson(const QList<TrackSpec> &specs)
{
  QJsonArray arr;
  for (const auto &spec : specs)
  {
    QJsonObject o;
    o.insert(QStringLiteral("type"), spec.typeId);
    o.insert(QStringLiteral("title"), spec.title);
    o.insert(QStringLiteral("width"), spec.width);
    o.insert(QStringLiteral("visible"), spec.visible);
    o.insert(QStringLiteral("print"), spec.printIncluded);
    o.insert(QStringLiteral("params"), QJsonObject::fromVariantMap(spec.params));
    arr << o;
  }
  return arr;
}

void WellCompositeStore::saveSessionTracks(const QString &project, const QString &well,
                                           const QList<TrackSpec> &specs)
{
  QSettings s;
  s.beginGroup(sessionKey(project, well));
  s.setValue(QStringLiteral("tracks"), QVariant::fromValue(specsToJson(specs).toVariantList()));
  s.endGroup();
}

QList<TrackSpec> WellCompositeStore::loadSessionTracks(const QString &project, const QString &well)
{
  QSettings s;
  s.beginGroup(sessionKey(project, well));
  const QVariant v = s.value(QStringLiteral("tracks"));
  s.endGroup();
  if (!v.isValid())
    return {};

  QJsonArray arr = QJsonArray::fromVariantList(v.toList());
  return specsFromJson(arr);
}

void WellCompositeStore::clearSessionTracks(const QString &project, const QString &well)
{
  QSettings s;
  s.remove(sessionKey(project, well));
}

void WellCompositeStore::saveWidthSet(const QString &project, const QString &well,
                                      const QVariantMap &titleToWidth)
{
  QSettings s;
  s.beginGroup(sessionKey(project, well));
  s.setValue(QStringLiteral("widths"), titleToWidth);
  s.endGroup();
}

QVariantMap WellCompositeStore::loadWidthSet(const QString &project, const QString &well)
{
  QSettings s;
  s.beginGroup(sessionKey(project, well));
  const QVariantMap m = s.value(QStringLiteral("widths")).toMap();
  s.endGroup();
  return m;
}

// ----------------------------------------------------------------------------
// 项目级 sidecar（JSON 文件）
// ----------------------------------------------------------------------------
WellCompositeStore::WellCompositeStore(const QString &sourceDataPath)
{
  // <源名>.<ext> → <源名>.wc.json（同目录；源数据永不被写）
  QString base = sourceDataPath;
  const int dot = base.lastIndexOf(QLatin1Char('.'));
  if (dot > 0)
    base.truncate(dot);
  m_sidecarPath = base + QStringLiteral(".wc.json");
}

bool WellCompositeStore::load()
{
  QFile f(m_sidecarPath);
  if (!f.exists())
    return true; // 空 sidecar 是合法状态
  if (!f.open(QIODevice::ReadOnly))
    return false;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  if (doc.isNull())
    return false;
  m_doc = doc.object();
  return true;
}

bool WellCompositeStore::save() const
{
  QFile f(m_sidecarPath);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  f.write(QJsonDocument(m_doc).toJson(QJsonDocument::Indented));
  return true;
}

namespace {

QJsonArray pinsToJson(const QList<DepthPin> &pins)
{
  QJsonArray arr;
  for (const auto &p : pins)
  {
    QJsonObject o;
    o.insert(QStringLiteral("depth"), p.depth);
    o.insert(QStringLiteral("text"), p.text);
    arr << o;
  }
  return arr;
}

QList<DepthPin> pinsFromJson(const QJsonArray &arr)
{
  QList<DepthPin> pins;
  for (const auto &v : arr)
  {
    DepthPin p;
    p.depth = v.toObject().value(QStringLiteral("depth")).toDouble();
    p.text = v.toObject().value(QStringLiteral("text")).toString();
    pins << p;
  }
  return pins;
}

QJsonArray bookmarksToJson(const QList<DepthBookmark> &bms)
{
  QJsonArray arr;
  for (const auto &b : bms)
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), b.name);
    o.insert(QStringLiteral("depth"), b.depth);
    arr << o;
  }
  return arr;
}

QList<DepthBookmark> bookmarksFromJson(const QJsonArray &arr)
{
  QList<DepthBookmark> bms;
  for (const auto &v : arr)
  {
    DepthBookmark b;
    b.name = v.toObject().value(QStringLiteral("name")).toString();
    b.depth = v.toObject().value(QStringLiteral("depth")).toDouble();
    bms << b;
  }
  return bms;
}

QJsonArray assignsToJson(const QList<StratAssignment> &as)
{
  QJsonArray arr;
  for (const auto &a : as)
  {
    QJsonObject o;
    o.insert(QStringLiteral("layer"), a.layerName);
    o.insert(QStringLiteral("system"), a.system);
    o.insert(QStringLiteral("series"), a.series);
    o.insert(QStringLiteral("formation"), a.formation);
    o.insert(QStringLiteral("member"), a.member);
    arr << o;
  }
  return arr;
}

QList<StratAssignment> assignsFromJson(const QJsonArray &arr)
{
  QList<StratAssignment> as;
  for (const auto &v : arr)
  {
    StratAssignment a;
    const QJsonObject o = v.toObject();
    a.layerName = o.value(QStringLiteral("layer")).toString();
    a.system = o.value(QStringLiteral("system")).toString();
    a.series = o.value(QStringLiteral("series")).toString();
    a.formation = o.value(QStringLiteral("formation")).toString();
    a.member = o.value(QStringLiteral("member")).toString();
    as << a;
  }
  return as;
}

} // namespace

QList<DepthPin> WellCompositeStore::pins() const
{
  return pinsFromJson(m_doc.value(QStringLiteral("pins")).toArray());
}

void WellCompositeStore::setPins(const QList<DepthPin> &pins)
{
  m_doc.insert(QStringLiteral("pins"), pinsToJson(pins));
}

QList<DepthBookmark> WellCompositeStore::bookmarks() const
{
  return bookmarksFromJson(m_doc.value(QStringLiteral("bookmarks")).toArray());
}

void WellCompositeStore::setBookmarks(const QList<DepthBookmark> &bookmarks)
{
  m_doc.insert(QStringLiteral("bookmarks"), bookmarksToJson(bookmarks));
}

QList<StratAssignment> WellCompositeStore::stratAssignments() const
{
  return assignsFromJson(m_doc.value(QStringLiteral("stratAssignments")).toArray());
}

void WellCompositeStore::setStratAssignments(const QList<StratAssignment> &assignments)
{
  m_doc.insert(QStringLiteral("stratAssignments"), assignsToJson(assignments));
}

QVariantMap WellCompositeStore::curveOverrides() const
{
  return m_doc.value(QStringLiteral("curveOverrides")).toObject().toVariantMap();
}

void WellCompositeStore::setCurveOverrides(const QVariantMap &overrides)
{
  m_doc.insert(QStringLiteral("curveOverrides"), QJsonObject::fromVariantMap(overrides));
}

QList<ExportPreset> WellCompositeStore::exportPresets() const
{
  QList<ExportPreset> ps;
  const QJsonArray arr = m_doc.value(QStringLiteral("exportPresets")).toArray();
  for (const auto &v : arr)
    ps << ExportPreset::fromVariantMap(v.toObject().toVariantMap());
  return ps;
}

void WellCompositeStore::setExportPresets(const QList<ExportPreset> &presets)
{
  QJsonArray arr;
  for (const auto &p : presets)
    arr << QJsonObject::fromVariantMap(p.toVariantMap());
  m_doc.insert(QStringLiteral("exportPresets"), arr);
}

QList<ComparisonTemplate> WellCompositeStore::comparisonTemplates() const
{
  QList<ComparisonTemplate> ts;
  const QJsonArray arr = m_doc.value(QStringLiteral("comparisonTemplates")).toArray();
  for (const auto &v : arr)
    ts << ComparisonTemplate::fromVariantMap(v.toObject().toVariantMap());
  return ts;
}

void WellCompositeStore::setComparisonTemplates(const QList<ComparisonTemplate> &templates)
{
  QJsonArray arr;
  for (const auto &t : templates)
    arr << QJsonObject::fromVariantMap(t.toVariantMap());
  m_doc.insert(QStringLiteral("comparisonTemplates"), arr);
}

void WellCompositeStore::appendAuditEntry(const QString &isoTimestamp, const QString &operation,
                                          const QString &detail)
{
  QJsonArray log = m_doc.value(QStringLiteral("auditLog")).toArray();
  QJsonObject entry;
  entry.insert(QStringLiteral("time"), isoTimestamp);
  entry.insert(QStringLiteral("op"), operation);
  entry.insert(QStringLiteral("detail"), detail);
  log << entry;
  m_doc.insert(QStringLiteral("auditLog"), log);
}

QJsonArray WellCompositeStore::auditLog() const
{
  return m_doc.value(QStringLiteral("auditLog")).toArray();
}

} // namespace WellComposite
