// 层：数据
#include "datacatalog.h"

#include "catalogstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

namespace
{

// #155：受管目录 artifacts/<stage>/ast-N/ver-M 的盘上最大序号。提交两步
// （先 rename 落位、后 applyJournal）之间崩溃会留下 catalog 不认识的孤儿目录；
// 若序号只按 catalog 推进，下一次导入会分到同一路径并被「拒绝覆盖」永久卡死。
// open 时把序号抬到盘上最大值之上即可避开（孤儿本身不删，只告警）。
void scanManagedSeqFloor(const QString &projectDir, int *maxAsset, int *maxVersion)
{
  const auto seqOf = [](const QString &name, QLatin1String prefix) -> int {
    if (!name.startsWith(prefix))
      return 0;
    bool ok = false;
    const int n = name.mid(prefix.size()).toInt(&ok);
    return ok && n > 0 ? n : 0;
  };
  const QDir artifacts(projectDir + QStringLiteral("/artifacts"));
  if (!artifacts.exists())
    return;
  const QStringList stages = artifacts.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString &stage : stages)
  {
    const QDir stageDir(artifacts.filePath(stage));
    const QStringList assets = stageDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &a : assets)
    {
      const int an = seqOf(a, QLatin1String("ast-"));
      if (an <= 0)
        continue;
      *maxAsset = qMax(*maxAsset, an);
      const QStringList versions =
          QDir(stageDir.filePath(a)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
      for (const QString &v : versions)
        *maxVersion = qMax(*maxVersion, seqOf(v, QLatin1String("ver-")));
    }
  }
}
  // wave/data-integrity：role 词表诊断标记。addLink/attachLink 把诊断写进
  // note 尾部，invalidRoleLinks() 按标记扫描——诊断因此随 catalog.json
  // round-trip，重开工程后诊断面仍可查。
  const QString kUnknownRoleMark = QStringLiteral("未知角色: ");
  const QString kRoleTypeMismatchMark = QStringLiteral("角色与实体类型不符: ");

  // 诚实降级：role 不在词表、或实体类型不在该 role 的 entityTypes——不拦
  // 不丢不改词（词表是工程自定义的，project_area.json 可扩；硬拦会把合法
  // 自定义挡在旧二进制外），只产出诊断文本（无违例回空）。
  // 空 entityType（调用方未给）或词表未声明挂接域（自定义角色可省
  // entity_types）无从核对，不诊断。
  QString roleDiagnosis(const EntityAssetLink &l, const RoleRegistry &roles)
  {
    const RoleDef *def = roles.find(l.role);
    if (!def)
      return kUnknownRoleMark + l.role;
    if (!l.entityType.isEmpty() && !def->entityTypes.isEmpty() &&
        !def->entityTypes.contains(l.entityType))
      return kRoleTypeMismatchMark +
             QStringLiteral("%1 于 %2").arg(l.role, l.entityType);
    return QString();
  }

  // 把诊断追加到 note 尾部（已有 note 用「；」连接）并 qWarning 一次。
  void annotateRoleDiagnostics(EntityAssetLink &l, const RoleRegistry &roles)
  {
    const QString diagnosis = roleDiagnosis(l, roles);
    if (diagnosis.isEmpty())
      return;
    l.note = l.note.isEmpty() ? diagnosis
                              : l.note + QStringLiteral("；") + diagnosis;
    qWarning() << "catalog:" << diagnosis;
  }

  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

} // namespace

DataCatalog::DataCatalog(QObject *parent)
  : QObject(parent)
{
}

DataCatalog::~DataCatalog() = default;

bool DataCatalog::ensureOpen(QString *error) const
{
  if (m_isOpen)
    return true;
  setError(error, QStringLiteral("catalog is not open"));
  return false;
}

// ---- 审计 02 M-8：线程纪律 + staging 副本 + journal 重放 ----
namespace
{
  std::atomic<int> g_threadViolations{0};

  bool strictThreads()
  {
    static const bool strict = qEnvironmentVariableIntValue("PALEO_CATALOG_STRICT_THREADS") != 0;
    return strict;
  }
} // namespace

int DataCatalog::threadViolationCount()
{
  return g_threadViolations.load();
}

void DataCatalog::resetThreadViolationCount()
{
  g_threadViolations.store(0);
}

bool DataCatalog::checkWriteThread(const char *what, QString *error) const
{
  // staging 副本归单个 worker 独占（无线程亲和）；活 catalog 只认所属线程。
  if (m_staging || !thread() || QThread::currentThread() == thread())
    return true;
  g_threadViolations.fetch_add(1);
  if (strictThreads())
    qFatal("DataCatalog::%s called off the catalog thread", what);
  qCritical("DataCatalog::%s refused: called off the catalog thread", what);
  setError(error, QStringLiteral("catalog 写入被拒绝：%1 不在 catalog 所属线程调用")
                      .arg(QString::fromLatin1(what)));
  return false;
}

void DataCatalog::noteRead(const char *what) const
{
  if (m_staging || !thread() || QThread::currentThread() == thread())
    return;
  const int n = g_threadViolations.fetch_add(1);
  if (strictThreads())
    qFatal("DataCatalog::%s read off the catalog thread", what);
  if (n < 8) // 只告警前几次——诊断面是计数器
    qWarning("DataCatalog::%s read off the catalog thread", what);
}

void DataCatalog::recordOp(CatalogOp op)
{
  ++m_mutationSeq;
  if (!m_staging)
    return;
  if (op.kind == CatalogOp::Kind::AddVersion && op.version.managed)
    m_overlayVersionIds.insert(op.version.id);
  m_journal.append(std::move(op));
}

std::unique_ptr<DataCatalog> DataCatalog::createStagingCopy(const QString &overlayDir) const
{
  noteRead("createStagingCopy");
  auto copy = std::make_unique<DataCatalog>();
  copy->m_dir = m_dir;
  copy->m_isOpen = m_isOpen;
  copy->m_openError = m_openError;
  copy->m_lockedReadOnly = m_lockedReadOnly;
  copy->m_revision = m_revision;
  copy->m_mutationSeq = m_mutationSeq;
  copy->m_entities = m_entities; // COW：worker 首次写才 detach
  copy->m_assets = m_assets;
  copy->m_versions = m_versions;
  copy->m_links = m_links;
  copy->m_assetSeq = m_assetSeq;
  copy->m_versionSeq = m_versionSeq;
  copy->m_roles = m_roles;
  copy->m_idx = m_idx;
  copy->m_backupKeep = m_backupKeep;
  copy->m_staging = true;
  copy->m_overlayDir = overlayDir;
  // m_store 不拷贝：副本默认空指针。save() 见 m_staging 即返回，不 open。
  // 无线程亲和：由领到它的 worker 独占使用，析构线程不限。
  copy->moveToThread(nullptr);
  return copy;
}

QString DataCatalog::versionFilePath(const CatalogVersion &v) const
{
  if (m_staging && v.managed && m_overlayVersionIds.contains(v.id))
    return resolvedVersionPath(m_overlayDir, v);
  return resolvedVersionPath(m_dir, v);
}

void DataCatalog::debugAbortJournalAfter(int k)
{
  m_debugAbortJournalAfter = k > 0 ? k : 0;
}

bool DataCatalog::applyJournal(const QVector<CatalogOp> &ops, QString *error)
{
  if (!checkWriteThread("applyJournal", error))
    return false;
  if (m_staging)
  {
    setError(error, QStringLiteral("staging 副本不能作为提交目标"));
    return false;
  }
  if (ops.isEmpty())
    return true;
  if (!ensureOpen(error))
    return false;
  // 事务快照：四表 COW + 序号 + 索引——失败时逐字段还原。
  const QVector<CatalogEntity> entities0 = m_entities;
  const QVector<CatalogAsset> assets0 = m_assets;
  const QVector<CatalogVersion> versions0 = m_versions;
  const QVector<EntityAssetLink> links0 = m_links;
  const int assetSeq0 = m_assetSeq, versionSeq0 = m_versionSeq;
  const CatalogIndex idx0 = m_idx;
  const int depth0 = m_batchDepth;
  const bool dirty0 = m_batchDirty;
  const QSet<QString> dirtyEntities0 = m_dirtyEntities;
  const QSet<QString> dirtyAssets0 = m_dirtyAssets;
  const QSet<QString> dirtyVersions0 = m_dirtyVersions;
  const QSet<int> dirtyLinkOrds0 = m_dirtyLinkOrds;
  const auto restore = [&] {
    m_entities = entities0;
    m_assets = assets0;
    m_versions = versions0;
    m_links = links0;
    m_assetSeq = assetSeq0;
    m_versionSeq = versionSeq0;
    m_idx = idx0;
    m_batchDepth = depth0;
    m_batchDirty = dirty0;
    m_dirtyEntities = dirtyEntities0;
    m_dirtyAssets = dirtyAssets0;
    m_dirtyVersions = dirtyVersions0;
    m_dirtyLinkOrds = dirtyLinkOrds0;
  };

  beginBatch();
  QString opErr;
  for (int i = 0; i < ops.size(); ++i)
  {
    const CatalogOp &op = ops.at(i);
    bool ok = false;
    switch (op.kind)
    {
      case CatalogOp::Kind::AddEntity: ok = addEntity(op.entity, &opErr); break;
      case CatalogOp::Kind::AddAsset: ok = addAsset(op.asset, &opErr); break;
      case CatalogOp::Kind::AddVersion: ok = addVersion(op.version, &opErr); break;
      case CatalogOp::Kind::AddLink: ok = addLink(op.link, &opErr); break;
      case CatalogOp::Kind::AttachLink: ok = attachLink(op.index, op.id, &opErr); break;
      case CatalogOp::Kind::SetLinkUnresolved: ok = setLinkUnresolved(op.index, &opErr); break;
      case CatalogOp::Kind::SetLinkPrimary: ok = setLinkPrimary(op.index, &opErr); break;
      case CatalogOp::Kind::MarkDownstreamStale:
        ok = markDownstreamStale(op.id, op.reason, &opErr);
        break;
    }
    if (!ok)
    {
      restore();
      setError(error, QStringLiteral("catalog 提交第 %1/%2 项失败：%3")
                          .arg(i + 1)
                          .arg(ops.size())
                          .arg(opErr.isEmpty() ? QStringLiteral("未给出原因") : opErr));
      return false;
    }
    // 注入中止必须在 endBatch/save 之前：此时还没有 sqlite 事务，-wal 字节才不变。
    if (m_debugAbortJournalAfter > 0 && (i + 1) == m_debugAbortJournalAfter)
    {
      restore();
      setError(error, QStringLiteral("catalog 提交在第 %1 个 op 后中止（测试注入）").arg(i + 1));
      return false;
    }
  }
  if (depth0 > 0)
    return endBatch(error); // 嵌套在外层批次里：由外层结算（外层失败语义归外层）
  QString saveErr;
  if (!endBatch(&saveErr))
  {
    restore();
    setError(error, saveErr.isEmpty() ? QStringLiteral("catalog 落盘失败") : saveErr);
    return false;
  }
  return true;
}

bool DataCatalog::open(const QString &projectDir, QString *error)
{
  ++m_mutationSeq; // 重开 = 整表替换
  m_isOpen = false;
  m_openError.clear();
  m_recoveredFromBackup = false; // 恢复态是「本次 open」的属性，重开重新判
  m_primaryCorruptOnDisk = false;
  m_backupRecoveryReason.clear();
  // 注意：m_lockedReadOnly 不在此重置——实例级只读降级由拥有者管理（见头注）。
  m_batchDepth = 0;
  m_batchDirty = false;
  m_batchSnapshot.reset();
  m_dir = projectDir.trimmed().isEmpty() ? QString() : projectDir;
  m_revision = 0;
  m_entities.clear();
  m_assets.clear();
  m_versions.clear();
  m_links.clear();
  m_assetSeq = m_versionSeq = 0;
  m_roles = RoleRegistry::defaults();
  m_idx.clear();
  m_dirtyEntities.clear();
  m_dirtyAssets.clear();
  m_dirtyVersions.clear();
  m_dirtyLinkOrds.clear();
  m_forceFullSave = false;

  const auto fail = [&](const QString &msg) {
    setError(error, msg);
    m_openError = msg; // 拒绝写入态的原因留存——openError() 供 UI 展示
    return false;
  };

  if (m_dir.isEmpty())
  {
    if (m_store)
      m_store->close();
    return fail(QStringLiteral("project directory is empty"));
  }

  // A 包角色词表：<projectDir>/project_area.json 的 roles 节做工程级覆盖；
  // 缺文件/解析失败/roles 非对象 → 静默留 defaults()。词表不是数据底座，
  // 缺它绝不阻塞工程打开（catalog.json 的成败不受影响）。
  QFile areaFile(m_dir + QStringLiteral("/project_area.json"));
  if (areaFile.open(QIODevice::ReadOnly))
  {
    const QJsonDocument areaDoc = QJsonDocument::fromJson(areaFile.readAll());
    if (areaDoc.isObject())
    {
      const QJsonValue roles = areaDoc.object().value(QStringLiteral("roles"));
      if (roles.isObject())
        m_roles = RoleRegistry::fromJson(roles.toObject());
    }
  }

  // 锁降级且盘上既没有 catalog.sqlite 也没有 catalog.json：openProject
  // 成功返回空表且不建文件（#80）。查询为空；refusesWrites() 仍为真，
  // 因为实例被锁，不是因为这次 open 失败。
  if (!m_store)
    m_store = std::make_unique<CatalogStore>();
  else
    m_store->close();

  QString serr;
  CatalogStore::Tables tables;
  if (!m_store->openProject(m_dir, m_lockedReadOnly, &tables, &serr))
  {
    m_store->close();
    return fail(serr.isEmpty() ? QStringLiteral("cannot open catalog") : serr);
  }

  m_entities = std::move(tables.entities);
  m_assets = std::move(tables.assets);
  m_versions = std::move(tables.versions);
  m_links = std::move(tables.links);
  m_revision = tables.meta.revision;
  m_assetSeq = qMax(m_assetSeq, tables.meta.assetSeq);
  m_versionSeq = qMax(m_versionSeq, tables.meta.versionSeq);
  {
    // #155：盘上孤儿受管目录也占号，见 scanManagedSeqFloor。
    int diskAsset = 0, diskVersion = 0;
    scanManagedSeqFloor(m_dir, &diskAsset, &diskVersion);
    if (diskAsset > m_assetSeq || diskVersion > m_versionSeq)
      qWarning("catalog: managed dirs on disk exceed catalog sequence (ast %d>%d, ver %d>%d)"
               " — orphaned files from an interrupted import commit; skipping those ids",
               diskAsset, m_assetSeq, diskVersion, m_versionSeq);
    m_assetSeq = qMax(m_assetSeq, diskAsset);
    m_versionSeq = qMax(m_versionSeq, diskVersion);
  }
  if (tables.meta.hasMutationSeq)
    m_mutationSeq = qMax(m_mutationSeq, tables.meta.mutationSeq + 1);
  if (tables.meta.hasBackupKeep)
    m_backupKeep = qBound(1, tables.meta.backupKeep, 9);
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links);

  if (m_store->recovered())
  {
    m_recoveredFromBackup = true;
    m_backupRecoveryReason = m_store->recoveryReason();
    m_primaryCorruptOnDisk = m_store->needsPrimaryRewrite();
    m_forceFullSave = m_primaryCorruptOnDisk && !m_lockedReadOnly;
    qWarning("catalog: recovered from backup (%s)", qPrintable(m_backupRecoveryReason));
    m_isOpen = true;
    emit backupRecovered(m_backupRecoveryReason);
    emit changed();
    return true;
  }

  m_recoveredFromBackup = false;
  m_primaryCorruptOnDisk = false;
  m_backupRecoveryReason.clear();
  m_forceFullSave = false;
  m_isOpen = true;
  emit changed();
  return true;
}

bool DataCatalog::exportCatalogJson(const QString &path, QString *error) const
{
  CatalogStore::Tables tables;
  tables.entities = m_entities;
  tables.assets = m_assets;
  tables.versions = m_versions;
  tables.links = m_links;
  tables.meta.revision = m_revision;
  const QByteArray bytes =
      QJsonDocument(CatalogStore::toJson(tables)).toJson(QJsonDocument::Indented);
  QSaveFile f(path);
  f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    return false;
  }
  if (f.write(bytes) != bytes.size())
  {
    const QString detail = f.errorString();
    f.cancelWriting();
    setError(error, QStringLiteral("short write to %1: %2").arg(path, detail));
    return false;
  }
  if (!f.commit())
  {
    setError(error, QStringLiteral("cannot replace %1: %2").arg(path, f.errorString()));
    return false;
  }
  return true;
}

bool DataCatalog::save(QString *error)
{
  if (!ensureOpen(error))
    return false;
  // 单写实例降级（§6）：锁在别的实例手里——本实例只读，任何落盘如实拒绝。
  // 必须在碰 store 之前返回，staging / 批次挂起同理。
  if (m_lockedReadOnly)
  {
    setError(error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，catalog 写入被拒绝"));
    return false;
  }
  // staging 副本：内存即结果，不落盘、不碰 store、不发 changed()。
  if (m_staging)
    return true;
  // 批量作用域内：只记脏、不落盘——endBatch 统一结算。
  if (m_batchDepth > 0)
  {
    m_batchDirty = true;
    return true;
  }
  return commitStore(error);
}

bool DataCatalog::commitStore(QString *error)
{
  const auto clearDirty = [this] {
    m_dirtyEntities.clear();
    m_dirtyAssets.clear();
    m_dirtyVersions.clear();
    m_dirtyLinkOrds.clear();
    m_removedAssets.clear();
    m_removedVersions.clear();
    m_linksFullRewrite = false;
  };
  if (!m_store)
  {
    clearDirty();
    setError(error, QStringLiteral("catalog store is not open"));
    return false;
  }

  CatalogStore::Meta meta;
  meta.revision = m_revision + 1;
  meta.mutationSeq = m_mutationSeq; // recordOp 在 save 成功之后才 +1
  meta.assetSeq = m_assetSeq;
  meta.versionSeq = m_versionSeq;
  meta.backupKeep = qBound(1, m_backupKeep, 9);
  meta.hasMutationSeq = true;
  meta.hasBackupKeep = true;

  if (m_forceFullSave || !m_store->isWritable())
  {
    CatalogStore::Tables tables;
    tables.entities = m_entities;
    tables.assets = m_assets;
    tables.versions = m_versions;
    tables.links = m_links;
    tables.meta = meta;
    if (!m_store->rewritePrimary(tables, m_primaryCorruptOnDisk, error))
    {
      clearDirty();
      return false;
    }
    m_forceFullSave = false;
    m_primaryCorruptOnDisk = false;
  }
  else
  {
    if (!m_store->begin(error))
    {
      m_store->rollback();
      clearDirty();
      return false;
    }
    const auto failTxn = [&] {
      m_store->rollback();
      clearDirty();
      return false;
    };
    // 脏行按行号升序写。QSet 迭代是哈希序（进程间随机）——新行将按该随机序
    // 进 sqlite 拿 rowid，重开（loadTables ORDER BY rowid）后表序 ≠ 内存表序，
    // 「表序最先」语义（versionBySha256 首命中、entities(type) 列表序）随进程
    // 抖动。升序 upsert 让增量落盘与 rewritePrimary 一样忠实序列化内存表序。
    const auto dirtyRows = [this](const QSet<QString> &ids,
                                  int (CatalogIndex::*rowOf)(const QString &) const) {
      QVector<int> rows;
      rows.reserve(ids.size());
      for (const QString &id : ids)
      {
        const int row = (m_idx.*rowOf)(id);
        if (row >= 0)
          rows.append(row);
      }
      std::sort(rows.begin(), rows.end());
      return rows;
    };
    for (const int row : dirtyRows(m_dirtyEntities, &CatalogIndex::entityRow))
      if (!m_store->upsertEntity(m_entities.at(row), error))
        return failTxn();
    for (const int row : dirtyRows(m_dirtyAssets, &CatalogIndex::assetRow))
      if (!m_store->upsertAsset(m_assets.at(row), error))
        return failTxn();
    for (const int row : dirtyRows(m_dirtyVersions, &CatalogIndex::versionRow))
      if (!m_store->upsertVersion(m_versions.at(row), error))
        return failTxn();
    QList<int> dirtyLinkOrds = m_dirtyLinkOrds.values();
    std::sort(dirtyLinkOrds.begin(), dirtyLinkOrds.end());
    for (const int ord : dirtyLinkOrds)
    {
      if (ord < 0 || ord >= m_links.size())
        continue;
      if (!m_store->upsertLink(ord, m_links.at(ord), error))
        return failTxn();
    }
    // 增量删除面（removeAsset）：先重写后删——同批「先改后删」的净效果 =
    // 已删；链接删行导致 ord 位移 → 整表重写收尾（upsert 过的行一并归位）。
    for (const QString &id : m_removedAssets)
      if (!m_store->deleteAsset(id, error))
        return failTxn();
    for (const QString &id : m_removedVersions)
      if (!m_store->deleteVersion(id, error))
        return failTxn();
    if (m_linksFullRewrite && !m_store->replaceAllLinks(m_links, error))
      return failTxn();
    if (!m_store->writeMeta(meta, error))
      return failTxn();
    if (!m_store->commit(error))
      return failTxn();
  }

  m_revision = meta.revision;
  clearDirty();
  m_batchDirty = false; // 真正落盘（batch depth 已是 0）才清
  emit changed();
  return true;
}

void DataCatalog::beginBatch()
{
  if (m_batchDepth == 0 && !m_staging)
  {
    auto snap = std::make_unique<BatchSnapshot>();
    snap->entities = m_entities;
    snap->assets = m_assets;
    snap->versions = m_versions;
    snap->links = m_links;
    snap->assetSeq = m_assetSeq;
    snap->versionSeq = m_versionSeq;
    snap->idx = m_idx;
    snap->dirtyEntities = m_dirtyEntities;
    snap->dirtyAssets = m_dirtyAssets;
    snap->dirtyVersions = m_dirtyVersions;
    snap->dirtyLinkOrds = m_dirtyLinkOrds;
    snap->removedAssets = m_removedAssets;
    snap->removedVersions = m_removedVersions;
    snap->linksFullRewrite = m_linksFullRewrite;
    m_batchSnapshot = std::move(snap);
  }
  ++m_batchDepth;
}

bool DataCatalog::endBatch(QString *error)
{
  if (m_batchDepth <= 0)
    return true; // 配对失衡由调用方栈结构保证，不 noisy
  if (--m_batchDepth > 0)
    return true; // 嵌套批次：只有最外层结算
  std::unique_ptr<BatchSnapshot> snap = std::move(m_batchSnapshot);
  if (!m_batchDirty)
    return true;
  if (save(error)) // 成功时 commitStore 才清 m_batchDirty
    return true;
  // #169：落盘失败——commitStore 已清脏集，批内改动若留在内存就永远不会
  // 再写盘（重开即静默丢失）。回滚内存到批次开始时，让内存与盘一致。
  if (snap)
  {
    m_entities = std::move(snap->entities);
    m_assets = std::move(snap->assets);
    m_versions = std::move(snap->versions);
    m_links = std::move(snap->links);
    m_assetSeq = snap->assetSeq;
    m_versionSeq = snap->versionSeq;
    m_idx = std::move(snap->idx);
    m_dirtyEntities = std::move(snap->dirtyEntities);
    m_dirtyAssets = std::move(snap->dirtyAssets);
    m_dirtyVersions = std::move(snap->dirtyVersions);
    m_dirtyLinkOrds = std::move(snap->dirtyLinkOrds);
    m_removedAssets = std::move(snap->removedAssets);
    m_removedVersions = std::move(snap->removedVersions);
    m_linksFullRewrite = snap->linksFullRewrite;
    m_batchDirty = false;
    ++m_mutationSeq;
    emit changed();
  }
  return false;
}

DataCatalog::BatchSave::BatchSave(DataCatalog *catalog)
  : m_catalog(catalog)
{
  if (m_catalog)
    m_catalog->beginBatch();
}

DataCatalog::BatchSave::~BatchSave()
{
  flush();
}

bool DataCatalog::BatchSave::flush(QString *error)
{
  if (m_done || !m_catalog)
    return true;
  m_done = true;
  return m_catalog->endBatch(error);
}

bool DataCatalog::addEntity(const CatalogEntity &e, QString *error)
{
  if (!checkWriteThread("addEntity", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (e.id.isEmpty() || hasEntity(e.id))
  {
    setError(error, QStringLiteral("entity id empty or duplicate: %1").arg(e.id));
    return false;
  }
  m_entities.append(e);
  m_idx.entityAdded(m_entities.size() - 1, e.id, e.entityType); // D5.2 增量
  m_dirtyEntities.insert(e.id);
  if (save(error))
  {
    CatalogOp op;
    op.kind = CatalogOp::Kind::AddEntity;
    op.entity = e;
    recordOp(std::move(op));
    return true;
  }
  m_entities.removeLast();
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links); // 回滚——罕见路径全量换正确性
  return false;
}

bool DataCatalog::addAsset(const CatalogAsset &a, QString *error)
{
  if (!checkWriteThread("addAsset", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (a.id.isEmpty() || !assetById(a.id).id.isEmpty())
  {
    setError(error, QStringLiteral("asset id is empty or duplicate: %1").arg(a.id));
    return false;
  }
  // T17 同型：显式给的 "ast-N" 也推进序号——否则 nextAssetId() 回发已用 id。
  {
    bool ok = false;
    const int n = a.id.startsWith(QStringLiteral("ast-")) ? a.id.mid(4).toInt(&ok) : 0;
    if (ok && n > 0)
      m_assetSeq = qMax(m_assetSeq, n);
  }
  m_assets.append(a);
  m_idx.assetAdded(m_assets.size() - 1, a.id, a.type); // D5.2
  m_dirtyAssets.insert(a.id);
  if (save(error))
  {
    CatalogOp op;
    op.kind = CatalogOp::Kind::AddAsset;
    op.asset = a;
    recordOp(std::move(op));
    return true;
  }
  m_assets.removeLast();
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links);
  return false;
}

bool DataCatalog::addVersion(const CatalogVersion &v, QString *error)
{
  if (!checkWriteThread("addVersion", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (v.id.isEmpty() || v.assetId.isEmpty())
  {
    setError(error, QStringLiteral("version id or asset id is empty"));
    return false;
  }
  if (!versionById(v.id).id.isEmpty())
  {
    setError(error, QStringLiteral("duplicate version id: %1").arg(v.id));
    return false;
  }
  if (assetById(v.assetId).id.isEmpty())
  {
    setError(error, QStringLiteral("version references unknown asset: %1").arg(v.assetId));
    return false;
  }
  // §3：fileName 与受管 path 的每一段都必须是合法路径段——catalog 不落坏段。
  // （外链 path 是文件系统绝对路径，含分隔符属正常，不查；空 path 表示未落位，
  // 交给调用方兜底。）
  if (!v.fileName.isEmpty() && !isSafePathSegment(v.fileName))
  {
    setError(error, QStringLiteral("unsafe path segment in version file name: %1").arg(v.fileName));
    return false;
  }
  if (!v.stage.isEmpty() && !isSafePathSegment(v.stage))
  {
    setError(error, QStringLiteral("unsafe path segment in version stage: %1").arg(v.stage));
    return false;
  }
  if (v.managed && !v.path.isEmpty())
  {
    for (const QString &seg : v.path.split(QLatin1Char('/')))
      if (!isSafePathSegment(seg))
      {
        setError(error, QStringLiteral("unsafe managed path segment: %1").arg(seg));
        return false;
      }
    if (resolvedVersionPath(m_dir, v).isEmpty())
    {
      setError(error, QStringLiteral("unsafe managed path: %1").arg(v.path));
      return false;
    }
  }
  // T17：显式给的 "ver-N" 也推进序号——不然 addVersion("ver-9") 之后
  // nextVersionId() 还会发 ver-9（被 dup 检查挡下报错）而不是发 ver-10。
  {
    bool ok = false;
    const int n = v.id.startsWith(QStringLiteral("ver-")) ? v.id.mid(4).toInt(&ok) : 0;
    if (ok && n > 0)
      m_versionSeq = qMax(m_versionSeq, n);
  }
  // WP2（catalog 写路径线性化）：不再全表快照 previousVersions——那会让
  // 每次 append 触发 QVector COW detach 的 O(N) 深拷贝（N 次导入即 O(N²)，
  // 100k 夹具实测 ~950s 的大头）。改精确 undo：新增行 removeLast 回退，
  // staleness 标记经 markStaleDownstreamOf 的 undo 栈逆序还原——save 失败
  // 仍是「版本与 stale 标记同进同退」的同一纪律，只是回退成本从 O(N)/次
  // 降到 O(undo)/次（undo 大小 = 下游闭包命中数，通常个位数）。
  m_versions.append(v);
  m_idx.versionAdded(m_versions.size() - 1, v); // D5.2
  // B 包 staleness-lite：新版本入库 = 同资产 versionNumber 更低的旧版本被
  // 取代（supersede）——其下游闭包中的 DERIVED 版本输入失效，随本次
  // addVersion 同一原子写落 extra["stale"]/["staleReason"]，不二次落盘。
  QStringList superseded;
  for (int r : m_idx.versionRowsForAsset(v.assetId)) // D5.1 O(命中集) 非全表
    if (m_versions.at(r).versionNumber < v.versionNumber)
      superseded.append(m_versions.at(r).id);
  QVector<QPair<int, CatalogVersion>> staleUndo;
  for (const QString &pid : superseded)
    markStaleDownstreamOf(
        pid, QStringLiteral("上游版本 %1 已被同资产新版本 %2 取代").arg(pid, v.id),
        &staleUndo);
  m_dirtyVersions.insert(v.id);
  if (save(error))
  {
    CatalogOp op;
    op.kind = CatalogOp::Kind::AddVersion;
    op.version = v;
    recordOp(std::move(op));
    return true;
  }
  for (int i = staleUndo.size() - 1; i >= 0; --i) // 逆序：同行的最早原值最后落
    m_versions[staleUndo[i].first] = staleUndo[i].second;
  m_versions.removeLast();
  m_idx.versionsMutated(m_versions);
  return false;
}

bool DataCatalog::addLink(const EntityAssetLink &l, QString *error)
{
  if (!checkWriteThread("addLink", error))
    return false;
  if (!ensureOpen(error))
    return false;
  // §3 修订：未决链接实体 id 留空（资产保留、不建不并）；已决链接仍必须有实体 id。
  if (l.assetId.isEmpty())
  {
    setError(error, QStringLiteral("link needs an asset id"));
    return false;
  }
  if (l.entityId.isEmpty() && !l.unresolved)
  {
    setError(error, QStringLiteral("resolved link needs an entity id"));
    return false;
  }
  // WP2：同 addVersion——全表快照换精确 undo（O(命中集)），失败回退路径
  // 语义不变：新链接摘除、被降级主关联复原。
  // 词表诊断先落 note（诚实降级——不拒收，见 invalidRoleLinks()）。
  EntityAssetLink stored = l;
  annotateRoleDiagnostics(stored, m_roles);
  m_links.append(stored);
  // §3：新的已决主关联入库后，同一 (entityType, entityId, role) 只保留这一条
  // 主关联——同角色旧主关联（例如同井同角色的旧版本资产）降级为非主。
  QVector<int> demotedRows;
  if (stored.isPrimary && !stored.unresolved)
    for (int i : m_idx.linkRowsForEntity(stored.entityId)) // D5.1 O(命中集)
      if (i != m_links.size() - 1 && m_links[i].isPrimary && !m_links[i].unresolved &&
          m_links[i].entityType == stored.entityType &&
          m_links[i].entityId == stored.entityId &&
          m_links[i].role == stored.role)
      {
        demotedRows.append(i);
        m_links[i].isPrimary = false;
      }
  m_dirtyLinkOrds.insert(m_links.size() - 1);
  for (int row : demotedRows)
    m_dirtyLinkOrds.insert(row);
  if (save(error))
  {
    m_idx.linkAdded(m_links.size() - 1, m_links.last()); // D5.2：纯追加——旧主关联
    CatalogOp op;                                         // 只降标志，邻接键不变
    op.kind = CatalogOp::Kind::AddLink;
    op.link = l; // 重放走同一 addLink（诊断/降级在目标 catalog 上重算）
    recordOp(std::move(op));
    return true;
  }
  for (int i : demotedRows)
    m_links[i].isPrimary = true;
  m_links.removeLast();
  m_idx.linksMutated(m_links);
  return false;
}

bool DataCatalog::attachLink(int index, const QString &entityId, QString *error)
{
  if (!checkWriteThread("attachLink", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (index < 0 || index >= m_links.size())
  {
    setError(error, QStringLiteral("link index out of range: %1").arg(index));
    return false;
  }
  // WP2：单行变更 + 命中集降级——快照换单行拷贝 + 降级行复原（O(命中集)）。
  const EntityAssetLink previousLink = m_links.at(index);
  EntityAssetLink &l = m_links[index];
  if (!l.unresolved)
  {
    setError(error, QStringLiteral("link %1 is not unresolved").arg(index));
    return false;
  }
  if (entityId.isEmpty())
  {
    setError(error, QStringLiteral("attach needs an entity id"));
    return false;
  }
  l.entityId = entityId;
  l.unresolved = false;
  l.isPrimary = true;
  l.note.clear();
  // 决议清空 note 后词表诊断重下——role 词表违例不因挂上实体而消失
  // （诚实降级，见 invalidRoleLinks()）。
  annotateRoleDiagnostics(l, m_roles);
  // 与 addLink 同一不变量：同一 (entityType, entityId, role) 只留这一条主关联。
  // 注意：未决链接也可能带 entityId（上游约定——entity 邻接先于决议建立），
  // 行集可能包含 index 自身，须显式排除。
  QVector<int> demotedRows;
  for (int i : m_idx.linkRowsForEntity(entityId)) // D5.1 O(命中集)
    if (i != index && m_links[i].isPrimary && !m_links[i].unresolved &&
        m_links[i].entityType == l.entityType && m_links[i].entityId == entityId &&
        m_links[i].role == l.role)
    {
      demotedRows.append(i);
      m_links[i].isPrimary = false;
    }
  m_dirtyLinkOrds.insert(index);
  for (int row : demotedRows)
    m_dirtyLinkOrds.insert(row);
  if (save(error))
  {
    m_idx.linksMutated(m_links); // entityId 获值——entity 邻接变化
    CatalogOp op;
    op.kind = CatalogOp::Kind::AttachLink;
    op.index = index;
    op.id = entityId;
    recordOp(std::move(op));
    return true;
  }
  m_links[index] = previousLink;
  for (int i : demotedRows)
    m_links[i].isPrimary = true;
  m_idx.linksMutated(m_links);
  return false;
}

bool DataCatalog::setLinkUnresolved(int index, QString *error)
{
  if (!checkWriteThread("setLinkUnresolved", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (index < 0 || index >= m_links.size())
  {
    setError(error, QStringLiteral("link index out of range: %1").arg(index));
    return false;
  }
  // WP2：单行变更——快照换单行拷贝复原。
  const EntityAssetLink previousLink = m_links.at(index);
  EntityAssetLink &l = m_links[index];
  if (l.unresolved)
  {
    setError(error, QStringLiteral("link %1 is already unresolved").arg(index));
    return false;
  }
  // 回退未决：实体 id 清空、不再持主关联。资产与被共享的井实体保留（§3）。
  l.entityId.clear();
  l.unresolved = true;
  l.isPrimary = false;
  l.note.clear();
  m_dirtyLinkOrds.insert(index);
  if (save(error))
  {
    m_idx.linksMutated(m_links); // entityId 被清空——entity 邻接变化
    CatalogOp op;
    op.kind = CatalogOp::Kind::SetLinkUnresolved;
    op.index = index;
    recordOp(std::move(op));
    return true;
  }
  m_links[index] = previousLink;
  m_idx.linksMutated(m_links);
  return false;
}

bool DataCatalog::setLinkPrimary(int index, QString *error)
{
  if (!checkWriteThread("setLinkPrimary", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (index < 0 || index >= m_links.size())
  {
    setError(error, QStringLiteral("link index out of range: %1").arg(index));
    return false;
  }
  // WP2：单行标志变更 + 命中集降级——快照换单行拷贝 + 降级行复原。
  const EntityAssetLink previousLink = m_links.at(index);
  EntityAssetLink &l = m_links[index];
  if (l.unresolved || l.entityId.isEmpty())
  {
    setError(error, QStringLiteral("link %1 is not a resolved link").arg(index));
    return false;
  }
  l.isPrimary = true;
  // 与 addLink/attachLink 同一不变量：同一 (entityType, entityId, role) 只留
  // 这一条主关联——同井同角色的旧版本资产降级为非主，不复制字节。
  // 行集是提升前的快照——index 行本就在 entity 行集里，须显式排除。
  QVector<int> demotedRows;
  for (int i : m_idx.linkRowsForEntity(l.entityId)) // D5.1 O(命中集)
    if (i != index && m_links[i].isPrimary && !m_links[i].unresolved &&
        m_links[i].entityType == l.entityType && m_links[i].entityId == l.entityId &&
        m_links[i].role == l.role)
    {
      demotedRows.append(i);
      m_links[i].isPrimary = false;
    }
  m_dirtyLinkOrds.insert(index);
  for (int row : demotedRows)
    m_dirtyLinkOrds.insert(row);
  if (save(error))
  {
    // 同上：纯标志位变化——零重索引。
    CatalogOp op;
    op.kind = CatalogOp::Kind::SetLinkPrimary;
    op.index = index;
    recordOp(std::move(op));
    return true;
  }
  m_links[index] = previousLink;
  for (int i : demotedRows)
    m_links[i].isPrimary = true;
  m_idx.linksMutated(m_links);
  return false;
}

// ---- 方向 30：物理删除资产（回收站「物理删除」的 catalog 面）----
bool DataCatalog::removeAsset(const QString &assetId, QString *error)
{
  if (!checkWriteThread("removeAsset", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (m_staging)
  {
    setError(error, QStringLiteral(
                        "staging copy does not support removeAsset (journal has no delete op)"));
    return false;
  }
  const CatalogAsset target = assetById(assetId);
  if (target.id.isEmpty())
  {
    setError(error, QStringLiteral("asset not found: %1").arg(assetId));
    return false;
  }
  // 血缘保护：待删版本被他资产的版本列为 parentVersionIds → 拒绝。
  // 同资产互引不拦（随资产一起删）。
  const QVector<CatalogVersion> doomedVersions = versionsForAsset(assetId);
  for (const CatalogVersion &v : doomedVersions)
    for (const QString &cid : m_idx.childVersionIds(v.id))
    {
      const int row = m_idx.versionRow(cid);
      if (row < 0 || m_versions.at(row).assetId == assetId)
        continue;
      setError(error, QStringLiteral("version %1 (v%2) is referenced as parent by "
                                     "asset %3 — delete the derived asset first")
                       .arg(v.id, QString::number(v.versionNumber),
                             m_versions.at(row).assetId));
      return false;
    }
  // 回滚快照：删除是罕见路径，O(N) 拷贝换 save 失败时的精确还原（与
  // 索引 rebuild 的「罕见路径花全量换正确性」同一纪律）。
  const QVector<CatalogAsset> prevAssets = m_assets;
  const QVector<CatalogVersion> prevVersions = m_versions;
  const QVector<EntityAssetLink> prevLinks = m_links;
  const CatalogIndex prevIdx = m_idx;
  const bool prevLinksFullRewrite = m_linksFullRewrite;
  for (int i = m_links.size() - 1; i >= 0; --i)
    if (m_links.at(i).assetId == assetId)
      m_links.removeAt(i);
  for (int i = m_versions.size() - 1; i >= 0; --i)
    if (m_versions.at(i).assetId == assetId)
      m_versions.removeAt(i);
  for (int i = m_assets.size() - 1; i >= 0; --i)
    if (m_assets.at(i).id == assetId)
      m_assets.removeAt(i);
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links);
  m_removedAssets.insert(assetId);
  for (const CatalogVersion &v : doomedVersions)
    m_removedVersions.insert(v.id);
  m_linksFullRewrite = true;
  if (save(error))
  {
    ++m_mutationSeq; // 同 mutator 口径：成功变更推进基线序号（无 journal op）
    return true;
  }
  m_assets = prevAssets;
  m_versions = prevVersions;
  m_links = prevLinks;
  m_idx = prevIdx;
  m_removedAssets.remove(assetId);
  for (const CatalogVersion &v : doomedVersions)
    m_removedVersions.remove(v.id);
  m_linksFullRewrite = prevLinksFullRewrite;
  return false;
}

// ---- wave/data-integrity：role 词表违例诊断面 ----
// 带「未知角色: / 角色与实体类型不符: 」note 标记的链接（addLink/attachLink
// 的诚实降级写入）。按标记扫描 note：诊断随 catalog.json round-trip，重开
// 后仍可查；词表内的干净链接（含普通未决备注）不在其中。
QVector<EntityAssetLink> DataCatalog::invalidRoleLinks() const
{
  QVector<EntityAssetLink> out;
  for (const EntityAssetLink &l : m_links)
    if (l.note.contains(kUnknownRoleMark) || l.note.contains(kRoleTypeMismatchMark))
      out.append(l);
  return out;
}

CatalogVersion DataCatalog::versionBySha256(const QString &sha256) const
{
  noteRead("versionBySha256");
  if (sha256.isEmpty())
    return CatalogVersion();
  // WP2：sha 命中行集走索引（旧实现线性全表扫——导入 dedup 每行调一次，
  // N 文件导入即 O(N²)）。行集升序＝表序——「第一个匹配」语义逐字节不变；
  // 命中后照旧复核文件在且字节一致（死路径旧记录不冒充命中）。
  const QVector<int> rows = m_idx.versionRowsForSha(sha256.toLower());
  for (int r : rows)
  {
    const CatalogVersion &v = m_versions.at(r);
    const QString path = versionFilePath(v); // staging：同批新增受管版本在暂存根
    if (path.isEmpty() || !QFileInfo(path).isFile()) continue;
    if (sha256FileHex(path).compare(sha256, Qt::CaseInsensitive) == 0)
      return v;
  }
  return CatalogVersion();
}

QString DataCatalog::resolvedVersionPath(const QString &projectDir, const CatalogVersion &version)
{
  if (version.path.isEmpty()) return QString();
  if (!version.managed) return version.path;
  if (QDir::isAbsolutePath(version.path) || version.path.contains(QLatin1Char('\\')))
    return QString();
  const QStringList segments = version.path.split(QLatin1Char('/'));
  for (const QString &segment : segments)
    if (!isSafePathSegment(segment)) return QString();

  const QString root = QFileInfo(projectDir).canonicalFilePath();
  if (root.isEmpty()) return QString();
  QString current = root;
  for (const QString &segment : segments)
  {
    current = QDir(current).filePath(segment);
    const QFileInfo info(current);
    if (info.isSymbolicLink()) return QString();
    if (info.exists())
    {
      // canonicalFilePath() 恒以 '/' 作分隔符（含 Windows）；QDir::separator()
      // 在 Windows 是 '\'，拼进前缀会让 startsWith 永假——曾致全部受管导入
      // 在 Windows 报 "unsafe managed path/destination"。
      const QString canonical = info.canonicalFilePath();
      if (!canonical.startsWith(root + QLatin1Char('/'))) return QString();
    }
  }
  return current;
}

QString DataCatalog::sha256FileHex(const QString &path, QString *error)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("cannot read %1").arg(path));
    return QString();
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  // 64KB chunks: a 1MB stack buffer overflows the default Windows thread
  // stack when this runs on the QTest main thread.
  char buf[64 << 10];
  qint64 n = 0;
  while ((n = f.read(buf, sizeof(buf))) > 0)
    hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
  if (n < 0)
  {
    setError(error, QStringLiteral("read error on %1").arg(path));
    return QString();
  }
  return QString::fromLatin1(hash.result().toHex());
}

bool DataCatalog::verifyExternalVersionSha(const CatalogVersion &v, QString *error) const
{
  // 受管版本（自己写入的副本）与未留底的外链（旧 catalog）无从校验。
  if (v.managed || v.sha256.isEmpty())
    return true;
  QString herr;
  const QString current = sha256FileHex(v.path, &herr); // 外链 path 是绝对路径
  if (current.isEmpty())
  {
    setError(error, herr.isEmpty() ? QStringLiteral("cannot read %1").arg(v.path) : herr);
    return false;
  }
  if (current.compare(v.sha256, Qt::CaseInsensitive) != 0)
  {
    setError(error, QStringLiteral("源文件与入库时的 SHA-256 不一致"));
    return false;
  }
  return true;
}

bool DataCatalog::isSafePathSegment(const QString &segment)
{
  // §3：一段路径拒绝空段、"."、任何含 ".." 的段、斜杠/反斜杠、NUL 与控制字符
  //（含换行、回车、Tab、DEL）。
  if (segment.isEmpty() || segment == QLatin1Char('.') ||
      segment.contains(QLatin1String("..")))
    return false;
  for (const QChar c : segment)
  {
    if (c == QLatin1Char('/') || c == QLatin1Char('\\'))
      return false;
    const ushort u = c.unicode();
    if (u < 0x20 || u == 0x7F)
      return false;
  }
  return true;
}

bool DataCatalog::hasEntity(const QString &id) const
{
  noteRead("hasEntity");
  return m_idx.entityRow(id) >= 0; // D5.1 O(1)
}

QVector<CatalogEntity> DataCatalog::entities(const QString &entityType) const
{
  noteRead("entities");
  QVector<CatalogEntity> out;
  if (entityType.isEmpty())
    return m_entities;
  const QVector<int> rows = m_idx.entityRowsByType(entityType); // D5.1
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_entities.at(r));
  return out;
}

CatalogEntity DataCatalog::entityById(const QString &id) const
{
  noteRead("entityById");
  const int row = m_idx.entityRow(id); // D5.1 O(1)
  return row >= 0 ? m_entities.at(row) : CatalogEntity();
}

QVector<CatalogAsset> DataCatalog::assets() const
{
  noteRead("assets");
  return m_assets;
}

CatalogAsset DataCatalog::assetById(const QString &id) const
{
  noteRead("assetById");
  const int row = m_idx.assetRow(id); // D5.1 O(1)
  return row >= 0 ? m_assets.at(row) : CatalogAsset();
}

QVector<CatalogVersion> DataCatalog::versionsForAsset(const QString &assetId) const
{
  noteRead("versionsForAsset");
  QVector<CatalogVersion> out;
  const QVector<int> rows = m_idx.versionRowsForAsset(assetId); // D5.1 O(1)+收集
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_versions.at(r));
  return out;
}

CatalogVersion DataCatalog::versionById(const QString &id) const
{
  noteRead("versionById");
  const int row = m_idx.versionRow(id); // D5.1 O(1)
  return row >= 0 ? m_versions.at(row) : CatalogVersion();
}

CatalogVersion DataCatalog::currentVersion(const QString &assetId) const
{
  noteRead("currentVersion");
  CatalogVersion best;
  const QVector<int> rows = m_idx.versionRowsForAsset(assetId); // D5.1
  for (int r : rows)
    if (m_versions.at(r).versionNumber >= best.versionNumber)
      best = m_versions.at(r);
  return best;
}

QVector<EntityAssetLink> DataCatalog::linksForEntity(const QString &entityId) const
{
  noteRead("linksForEntity");
  // audit row 35：空 id 不等于「全部未决链接」——未决集合走 unresolvedLinks()；
  // 这里如实返回空集，不然调用方拿空串查询会静默命中全部未决链接。
  if (entityId.isEmpty())
    return {};
  QVector<EntityAssetLink> out;
  const QVector<int> rows = m_idx.linkRowsForEntity(entityId); // D5.1（行升序）
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_links.at(r));
  // B 包：同 (entity,role) 成员按 ordinal 升序展示。stable_sort 只按 ordinal
  // 排——不同角色间的相对序保持入库序；旧数据 ordinal 全 0 时输出与排序前
  // 完全一致，对既有消费方零扰动。
  std::stable_sort(out.begin(), out.end(),
                   [](const EntityAssetLink &a, const EntityAssetLink &b) {
                     return a.ordinal < b.ordinal;
                   });
  return out;
}

QVector<EntityAssetLink> DataCatalog::unresolvedLinks() const
{
  QVector<EntityAssetLink> out;
  for (const EntityAssetLink &l : m_links)
    if (l.unresolved)
      out.append(l);
  return out;
}

QVector<EntityAssetLink> DataCatalog::linksForAsset(const QString &assetId) const
{
  noteRead("linksForAsset");
  QVector<EntityAssetLink> out;
  const QVector<int> rows = m_idx.linkRowsForAsset(assetId); // D5.1（行升序）
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_links.at(r));
  return out;
}

QVector<EntityAssetLink> DataCatalog::links() const
{
  noteRead("links");
  return m_links;
}

QVector<CatalogVersion> DataCatalog::downstreamClosure(const QString &versionId) const
{
  QVector<CatalogVersion> out;
  if (versionId.isEmpty())
    return out;
  // BFS：邻接表反查（D5.4 childrenByParent）——不再每层全表扫 m_versions。
  // 结果序 = BFS 发现序（frontier 序 × 子版本行序），与旧「全表扫描 × 表序」
  // 逐项一致（子版本按行序入边）。seen 先放种子——环（A→B→A）里种子不作为
  // 「自己的下游」进结果，也保证遍历终止。
  QSet<QString> seen;
  seen.insert(versionId);
  QStringList frontier{versionId};
  for (int head = 0; head < frontier.size(); ++head)
  {
    // 值拷贝：frontier.append 可能重分配 QList 存储，引用会悬垂。
    const QString cur = frontier.at(head);
    for (const QString &childId : m_idx.childVersionIds(cur))
    {
      if (seen.contains(childId))
        continue;
      const int row = m_idx.versionRow(childId);
      if (row < 0)
        continue;
      seen.insert(childId);
      frontier.append(childId);
      out.append(m_versions.at(row));
    }
  }
  return out;
}

int DataCatalog::markStaleDownstreamOf(const QString &versionId, const QString &reason,
                                       QVector<QPair<int, CatalogVersion>> *undo)
{
  const QVector<CatalogVersion> downstream = downstreamClosure(versionId);
  int changed = 0;
  for (const CatalogVersion &d : downstream)
  {
    // staleness-lite：只标 DERIVED 产物。闭包里的 INTERMEDIATE/OUTPUT 等
    // 参与溯源穿透（DERIVED 的祖先可以是任意阶段），但自身不记 stale。
    if (d.stage != QLatin1String("DERIVED"))
      continue;
    // WP2：行号走邻接索引 O(1)——旧实现每次全表建 id→row QHash，是
    // addVersion 路径隐藏的 O(N)/调用（N 次导入即 O(N²)）。
    const int i = m_idx.versionRow(d.id);
    if (i < 0)
      continue; // 闭包快照自洽——防御性跳过
    CatalogVersion &m = m_versions[i];
    if (m.extra.value(QStringLiteral("stale")).toBool() &&
        m.extra.value(QStringLiteral("staleReason")).toString() == reason)
      continue; // 同一标记已在——不算变更（幂等，不空涨 revision）
    if (undo)
      undo->append({i, m});
    m.extra.insert(QStringLiteral("stale"), true);
    m.extra.insert(QStringLiteral("staleReason"), reason);
    m_dirtyVersions.insert(m.id);
    ++changed;
  }
  return changed;
}

bool DataCatalog::markDownstreamStale(const QString &versionId, const QString &reason,
                                      QString *error)
{
  if (!checkWriteThread("markDownstreamStale", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (versionId.isEmpty())
  {
    setError(error, QStringLiteral("cannot mark downstream of an empty version id"));
    return false;
  }
  if (versionById(versionId).id.isEmpty())
  {
    setError(error,
             QStringLiteral("cannot mark downstream of unknown version: %1").arg(versionId));
    return false;
  }
  const QString why =
      reason.isEmpty() ? QStringLiteral("上游版本源已失效") : reason;
  // WP2：全表快照换 undo 栈（O(命中行)）——落盘失败逆序还原，纪律不变。
  QVector<QPair<int, CatalogVersion>> undo;
  if (markStaleDownstreamOf(versionId, why, &undo) == 0)
    return true; // 无下游或标记未变——不落盘、不空涨 revision
  if (save(error))
  {
    CatalogOp op;
    op.kind = CatalogOp::Kind::MarkDownstreamStale;
    op.id = versionId;
    op.reason = reason;
    recordOp(std::move(op));
    return true;
  }
  for (int i = undo.size() - 1; i >= 0; --i)
    m_versions[undo[i].first] = undo[i].second; // 落盘失败回滚内存
  return false;
}

QString DataCatalog::normalizeWellName(const QString &name)
{
  QString out;
  out.reserve(name.size());
  for (const QChar c : name)
  {
    if (c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char('_'))
      continue;
    out.append(c.toLower());
  }
  return out;
}

QStringList DataCatalog::wellsMatchingName(const QString &name) const
{
  noteRead("wellsMatchingName");
  const QString needle = normalizeWellName(name);
  QStringList out;
  if (needle.isEmpty())
    return out;
  const QVector<int> rows = m_idx.entityRowsByType(QStringLiteral("well")); // D5.1
  for (int r : rows)
  {
    // D12：井身份只走规范化 name——uwi/别名匹配已随字段一并剥离。
    if (normalizeWellName(m_entities.at(r).name) == needle)
      out.append(m_entities.at(r).id);
  }
  return out;
}

QString DataCatalog::managedPath(const QString &stage, const QString &assetId,
                                 const QString &versionId, const QString &fileName)
{
  // §3：任一段不是合法路径段就回空串——catalog 拒绝产出坏路径。
  if (!isSafePathSegment(stage) || !isSafePathSegment(assetId) ||
      !isSafePathSegment(versionId) || !isSafePathSegment(fileName))
    return QString();
  return QStringLiteral("%1/%2/%3/%4").arg(stage.toLower(), assetId, versionId, fileName);
}

QString DataCatalog::nextAssetId()
{
  noteRead("nextAssetId");
  ++m_mutationSeq; // 分配 id 也是 owner 侧状态变化（提交基线要看见）
  QString id;
  do { id = QStringLiteral("ast-%1").arg(++m_assetSeq); }
  while (!assetById(id).id.isEmpty());
  return id;
}

QString DataCatalog::nextVersionId()
{
  noteRead("nextVersionId");
  ++m_mutationSeq; // 分配 id 也是 owner 侧状态变化（提交基线要看见）
  QString id;
  do { id = QStringLiteral("ver-%1").arg(++m_versionSeq); }
  while (!versionById(id).id.isEmpty());
  return id;
}

QString DataCatalog::nextEntityId(const QString &prefix)
{
  noteRead("nextEntityId");
  ++m_mutationSeq; // 分配 id 也是 owner 侧状态变化（提交基线要看见）
  // WP2：前缀最大序号走索引（旧实现线性扫全实体表——导入井/辅助实体逐个
  // 调用即 O(N²)）。语义等价：只认「prefix 后跟 '-' 且余段纯数字」的既有 id。
  const int max = m_idx.maxEntitySeqForPrefix(prefix);
  return QStringLiteral("%1-%2").arg(prefix).arg(max + 1);
}

bool DataCatalog::writeWellsGeoJson(const QString &path, QString *error) const
{
  // §4 井位图层数据源：只写有 surface 坐标且坐标有限的井；坐标是原始
  // surface_x/y（局部测网米），真投影参数出现前地图一直读它。
  QJsonArray feats;
  for (const CatalogEntity &e : m_entities)
  {
    if (e.entityType != QLatin1String("well") || !e.hasSurface)
      continue;
    if (!std::isfinite(e.surfaceX) || !std::isfinite(e.surfaceY))
      continue;
    QJsonObject props;
    props.insert(QStringLiteral("id"), e.id);
    props.insert(QStringLiteral("name"), e.name);
    props.insert(QStringLiteral("coordinate_status"), e.coordinateStatus);
    QJsonObject geom;
    geom.insert(QStringLiteral("type"), QStringLiteral("Point"));
    geom.insert(QStringLiteral("coordinates"), QJsonArray{e.surfaceX, e.surfaceY});
    QJsonObject f;
    f.insert(QStringLiteral("type"), QStringLiteral("Feature"));
    f.insert(QStringLiteral("properties"), props);
    f.insert(QStringLiteral("geometry"), geom);
    feats.append(f);
  }
  if (feats.isEmpty())
    return true; // 没有可定位的井——不写空文件，也不算失败

  QJsonObject root;
  root.insert(QStringLiteral("type"), QStringLiteral("FeatureCollection"));
  QJsonObject crsProps;
  crsProps.insert(QStringLiteral("name"), localGridCrsWkt());
  QJsonObject crs;
  crs.insert(QStringLiteral("type"), QStringLiteral("name"));
  crs.insert(QStringLiteral("properties"), crsProps);
  root.insert(QStringLiteral("crs"), crs);
  root.insert(QStringLiteral("features"), feats);

  QDir().mkpath(QFileInfo(path).absolutePath());
  // 原子写审计（T6）：井点 GeoJSON 是 wells 图层的数据源——裸 QFile 截断
  // 写中途崩溃会留下半截文件，OGR 照常打开但要素缺失（静默坏图层）。
  // QSaveFile 全量写成功才替换，坏盘/短写保住上一份好文件。
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, tr("井点 GeoJSON 写入失败：%1（%2）").arg(path, file.errorString()));
    return false;
  }
  if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0 ||
      !file.commit())
  {
    setError(error, tr("井点 GeoJSON 写入失败：%1").arg(path));
    return false;
  }
  return true;
}

// ---- D5 计数缓存 / 索引健康 ----
QHash<QString, int> DataCatalog::entityCountsByType() const
{
  return m_idx.entityCountsByType();
}

bool DataCatalog::indexHealthy(QString *mismatch) const
{
  return m_idx.verifyAgainst(m_entities, m_assets, m_versions, m_links, mismatch);
}
