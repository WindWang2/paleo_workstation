// 层：功能
#include "importledger.h"

#include "../catalog/datacatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QVariantList>

namespace paleo::imports
{

namespace
{

// <projectDir>/artifacts/metadata/catalog.json → 工程目录 = 文件上三级
// （与 dataops sidecar 的修正后口径一致；纯词法，不查盘）。
QString projectDirForLedger(DataCatalog *cat)
{
  if (!cat || !cat->isOpen())
    return QString();
  const QString cp = cat->catalogPath();
  if (cp.isEmpty())
    return QString();
  return QFileInfo(QFileInfo(QFileInfo(cp).dir().absolutePath()).dir().absolutePath())
      .dir()
      .absolutePath();
}

QString stringOrEmpty(const QVariantMap &m, const char *key)
{
  return m.value(QLatin1String(key)).toString();
}

} // namespace

QVariantMap LedgerBatch::toVariant() const
{
  QVariantMap m;
  m.insert(QStringLiteral("id"), id);
  m.insert(QStringLiteral("dir"), dir);
  m.insert(QStringLiteral("startedAt"), startedAt.toString(Qt::ISODate));
  m.insert(QStringLiteral("finishedAt"), finishedAt.toString(Qt::ISODate));
  m.insert(QStringLiteral("imported"), imported);
  m.insert(QStringLiteral("unresolved"), unresolved);
  m.insert(QStringLiteral("failed"), failed);
  m.insert(QStringLiteral("skipped"), skipped);
  m.insert(QStringLiteral("error"), error);
  QVariantList rowList;
  for (const LedgerRow &r : rows)
  {
    QVariantMap rm;
    rm.insert(QStringLiteral("path"), r.path);
    rm.insert(QStringLiteral("type"), r.type);
    rm.insert(QStringLiteral("entity"), r.entity);
    rm.insert(QStringLiteral("outcome"), r.outcome);
    rm.insert(QStringLiteral("message"), r.message);
    rowList.append(rm);
  }
  m.insert(QStringLiteral("rows"), rowList);
  return m;
}

LedgerBatch LedgerBatch::fromVariant(const QVariantMap &m)
{
  LedgerBatch b;
  b.id = stringOrEmpty(m, "id");
  b.dir = stringOrEmpty(m, "dir");
  b.startedAt = QDateTime::fromString(stringOrEmpty(m, "startedAt"), Qt::ISODate);
  b.finishedAt = QDateTime::fromString(stringOrEmpty(m, "finishedAt"), Qt::ISODate);
  b.imported = m.value(QLatin1String("imported")).toInt();
  b.unresolved = m.value(QLatin1String("unresolved")).toInt();
  b.failed = m.value(QLatin1String("failed")).toInt();
  b.skipped = m.value(QLatin1String("skipped")).toInt();
  b.error = stringOrEmpty(m, "error");
  const QVariantList rowList = m.value(QLatin1String("rows")).toList();
  for (const QVariant &v : rowList)
  {
    const QVariantMap rm = v.toMap();
    if (!rm.contains(QStringLiteral("path")))
      continue;
    LedgerRow r;
    r.path = stringOrEmpty(rm, "path");
    r.type = stringOrEmpty(rm, "type");
    r.entity = stringOrEmpty(rm, "entity");
    r.outcome = stringOrEmpty(rm, "outcome");
    r.message = stringOrEmpty(rm, "message");
    b.rows.append(r);
  }
  return b;
}

void ImportLedger::load(DataCatalog *cat)
{
  m_batches.clear();
  m_path = QString();
  const QString pd = projectDirForLedger(cat);
  if (pd.isEmpty())
    return;
  m_path = QDir(pd).filePath(QStringLiteral(".paleo/import_ledger.json"));
  QFile f(m_path);
  if (!f.open(QIODevice::ReadOnly))
    return; // 无台账 = 空历史，不是错误
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
  {
    qWarning() << "import ledger parse failed:" << m_path << err.errorString();
    return;
  }
  const QJsonArray arr = doc.object().value(QStringLiteral("batches")).toArray();
  for (const auto &v : arr)
  {
    LedgerBatch b = LedgerBatch::fromVariant(v.toObject().toVariantMap());
    if (!b.id.isEmpty())
      m_batches.append(b);
  }
}

bool ImportLedger::record(const LedgerBatch &batch, QString *error)
{
  if (m_path.isEmpty())
  {
    if (error)
      *error = QStringLiteral("导入台账未激活（工程 catalog 未打开）");
    return false;
  }
  m_batches.append(batch);
  // 窗口裁剪：保最新 kMaxBatches 批（append 序即时间序）。
  while (m_batches.size() > kMaxBatches)
    m_batches.removeFirst();

  QJsonObject root;
  QJsonArray arr;
  for (const LedgerBatch &b : m_batches)
  {
    QJsonObject o;
    const QVariantMap m = b.toVariant();
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
    {
      const QVariant v = it.value();
      if (v.type() == QVariant::List)
      {
        QJsonArray a;
        for (const QVariant &rv : v.toList())
          a.append(QJsonObject::fromVariantMap(rv.toMap()));
        o.insert(it.key(), a);
      }
      else
        o.insert(it.key(), QJsonValue::fromVariant(v));
    }
    arr.append(o);
  }
  root.insert(QStringLiteral("batches"), arr);

  QDir().mkpath(QFileInfo(m_path).absolutePath());
  QSaveFile f(m_path);
  if (!f.open(QIODevice::WriteOnly))
  {
    if (error)
      *error = QStringLiteral("无法写入导入台账 %1：%2")
                   .arg(m_path, f.errorString());
    return false;
  }
  f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
  if (!f.commit())
  {
    if (error)
      *error = QStringLiteral("导入台账落盘失败 %1：%2")
                   .arg(m_path, f.errorString());
    return false;
  }
  return true;
}

} // namespace paleo::imports
