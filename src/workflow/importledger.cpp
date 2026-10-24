// 层：功能
#include "importledger.h"

#include "../catalog/datacatalog.h"

#include <QDateTime>
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

// opt-01 P2：台账在 GUI 线程上 load+record，旧实现经 QVariantMap/QVariantList
// 中转（每行一个 QVariantMap，再 fromVariantMap/toVariantMap），10 万行级批次
// 实测秒级卡顿。下面两函数直接在 QJsonObject 与 LedgerBatch 间转换，字段、
// 类型与容错口径与 toVariant/fromVariant 逐一对应（落盘字节相同，见
// tst_io_importledger::ledgerScaleRoundTrip 的 sha256 交叉校验）。
QString jsonString(const QJsonObject &o, QLatin1String key)
{
  const QJsonValue v = o.value(key);
  if (v.isString())
    return v.toString();
  return v.toVariant().toString(); // 与 QVariant::toString 同口径（数字 → 文本）
}

QJsonObject batchToJson(const LedgerBatch &b)
{
  QJsonObject o;
  o.insert(QLatin1String("id"), b.id);
  o.insert(QLatin1String("dir"), b.dir);
  o.insert(QLatin1String("startedAt"), b.startedAt.toString(Qt::ISODate));
  o.insert(QLatin1String("finishedAt"), b.finishedAt.toString(Qt::ISODate));
  o.insert(QLatin1String("imported"), b.imported);
  o.insert(QLatin1String("unresolved"), b.unresolved);
  o.insert(QLatin1String("failed"), b.failed);
  o.insert(QLatin1String("skipped"), b.skipped);
  o.insert(QLatin1String("error"), b.error);
  QJsonArray rows;
  for (const LedgerRow &r : b.rows)
  {
    QJsonObject ro;
    ro.insert(QLatin1String("path"), r.path);
    ro.insert(QLatin1String("type"), r.type);
    ro.insert(QLatin1String("entity"), r.entity);
    ro.insert(QLatin1String("outcome"), r.outcome);
    ro.insert(QLatin1String("message"), r.message);
    rows.append(ro);
  }
  o.insert(QLatin1String("rows"), rows);
  return o;
}

LedgerBatch batchFromJson(const QJsonObject &o)
{
  LedgerBatch b;
  b.id = jsonString(o, QLatin1String("id"));
  b.dir = jsonString(o, QLatin1String("dir"));
  b.startedAt = QDateTime::fromString(jsonString(o, QLatin1String("startedAt")), Qt::ISODate);
  b.finishedAt = QDateTime::fromString(jsonString(o, QLatin1String("finishedAt")), Qt::ISODate);
  b.imported = o.value(QLatin1String("imported")).toVariant().toInt();
  b.unresolved = o.value(QLatin1String("unresolved")).toVariant().toInt();
  b.failed = o.value(QLatin1String("failed")).toVariant().toInt();
  b.skipped = o.value(QLatin1String("skipped")).toVariant().toInt();
  b.error = jsonString(o, QLatin1String("error"));
  const QJsonArray rows = o.value(QLatin1String("rows")).toArray();
  b.rows.reserve(rows.size());
  for (const QJsonValue &v : rows)
  {
    const QJsonObject ro = v.toObject();
    if (!ro.contains(QLatin1String("path")))
      continue;
    LedgerRow r;
    r.path = jsonString(ro, QLatin1String("path"));
    r.type = jsonString(ro, QLatin1String("type"));
    r.entity = jsonString(ro, QLatin1String("entity"));
    r.outcome = jsonString(ro, QLatin1String("outcome"));
    r.message = jsonString(ro, QLatin1String("message"));
    b.rows.append(r);
  }
  return b;
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
  m_loadWarning.clear();
  const QString pd = projectDirForLedger(cat);
  if (pd.isEmpty())
    return;
  m_path = QDir(pd).filePath(QStringLiteral(".paleo/import_ledger.json"));
  QFile f(m_path);
  if (!f.exists())
    return; // 无台账 = 空历史，不是错误
  if (!f.open(QIODevice::ReadOnly))
  {
    // 文件在但读不了（权限/占用）：历史不可见，不能当成「空台账」——否则下次
    // record 会用一批覆盖全部历史。按损坏口径：先备份，备份不了即拒写。
    qWarning() << "import ledger unreadable:" << m_path << f.errorString();
    m_loadWarning = QStringLiteral("导入台账无法读取（%1），历史批次不可见；"
                                   "下次写入前将先备份原文件，无法备份则不写入")
                        .arg(f.errorString());
    return;
  }
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
  {
    qWarning() << "import ledger parse failed:" << m_path << err.errorString();
    m_loadWarning = QStringLiteral("导入台账已损坏（%1），历史批次无法读取；"
                                   "下次写入前将原文件另存为 .corrupt 备份")
                        .arg(doc.isNull() ? err.errorString()
                                          : QStringLiteral("根节点不是对象"));
    return;
  }
  const QJsonValue batchesValue = doc.object().value(QStringLiteral("batches"));
  if (!batchesValue.isArray())
  {
    // 合法 JSON 但没有 batches 数组：不是本台账的形状，同样不得被静默覆盖。
    qWarning() << "import ledger has no batches array:" << m_path;
    m_loadWarning = QStringLiteral("导入台账已损坏（缺少批次数组），历史批次无法读取；"
                                   "下次写入前将原文件另存为 .corrupt 备份");
    return;
  }
  const QJsonArray arr = batchesValue.toArray();
  for (const auto &v : arr)
  {
    LedgerBatch b = batchFromJson(v.toObject());
    if (!b.id.isEmpty())
      m_batches.append(b);
  }
  if (m_batches.size() < arr.size())
  {
    // 有批次无法识别（全部或部分）：保留可识别的批次照常显示，但下一次 record
    // 会只写回可识别的部分——原文件须先备份（S5 第 2 轮 Low 9：部分无效也算）。
    qWarning() << "import ledger:" << arr.size() - m_batches.size() << "of" << arr.size()
               << "batches unrecognised" << m_path;
    m_loadWarning = QStringLiteral("导入台账已损坏（%1 / %2 个批次无法识别），无法识别的批次未显示；"
                                   "下次写入前将原文件另存为 .corrupt 备份")
                        .arg(arr.size() - m_batches.size())
                        .arg(arr.size());
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
  if (!m_loadWarning.isEmpty() && QFile::exists(m_path))
  {
    // 损坏件是唯一的历史载体：覆盖前先保全原字节（不就地修复、不猜内容）。
    // 另存失败 → 拒写，宁可本批不入账，也不把全部历史换成一批。
    const QString backup =
        m_path + QStringLiteral(".corrupt-") +
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz"));
    // 备份经 QSaveFile（临时文件 + 落盘同步 + 改名）写出：主台账随后被覆盖，
    // 备份必须先于覆盖真正落盘，掉电时不能两头都丢（S5 U2-b）。
    QFile original(m_path);
    QSaveFile out(backup);
    const bool saved = original.open(QIODevice::ReadOnly) &&
                       out.open(QIODevice::WriteOnly) &&
                       [&] {
                         const QByteArray bytes = original.readAll();
                         return out.write(bytes) == bytes.size();
                       }() &&
                       out.commit();
    if (!saved)
    {
      if (error)
        *error = QStringLiteral("导入台账已损坏且无法备份（%1），本批未入账以免覆盖原文件")
                     .arg(backup);
      return false;
    }
    qWarning() << "import ledger: corrupt ledger preserved as" << backup;
    m_loadWarning.clear();
  }
  m_batches.append(batch);
  // 窗口裁剪：保最新 kMaxBatches 批（append 序即时间序）。
  while (m_batches.size() > kMaxBatches)
    m_batches.removeFirst();

  QJsonObject root;
  QJsonArray arr;
  for (const LedgerBatch &b : m_batches)
    arr.append(batchToJson(b));
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
  // 短写必须当失败：显式校验字节数，失败即放弃本次提交（旧台账保持完好）。
  const QByteArray ledgerBytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  if (f.write(ledgerBytes) != ledgerBytes.size())
  {
    const QString detail = f.errorString();
    f.cancelWriting();
    if (error)
      *error = QStringLiteral("导入台账写入不完整 %1：%2").arg(m_path, detail);
    return false;
  }
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
