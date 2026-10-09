// 层：数据
#include "datacatalog.h"
#include "../metadata/storeerrors_internal.h"
#include "purgelease.h"

#include "catalogstore.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QThread>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <memory>

// datacatalog 主 TU（方向 99 拆分后）：生命周期（open/prepareOpen/
// adoptPrepared）、线程纪律与 staging 副本/journal 重放（审计 02 M-8）、
// 落盘面（save/commitStore 增量事务/BatchSave 批次快照）、D5 索引诊断。
// 查询族与 mutator 按 _entities/_assets/_links/_versions/_derivation/
// _export 六 TU 分治（切分线见 .goal-loop-ledger-catalog-split.md）。

namespace
{

using paleo::store_detail::setError;

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
  if (!m_staging && CatalogPurgeLease::isHeld(catalogPath())) {
    setError(error, tr("文件回收提交中，请等待回收完成后再修改目录。"));
    return false;
  }
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
      case CatalogOp::Kind::UpdateVersionExtra:
        ok = updateVersionExtra(op.id, op.extraKey, op.extraValue, &opErr);
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

std::shared_ptr<DataCatalog> DataCatalog::prepareOpen(const QString &projectDir, bool readOnly)
{
  auto prepared = std::make_shared<DataCatalog>();
  prepared->setLockedReadOnly(readOnly);
  QString error;
  if (!prepared->open(projectDir, &error)) prepared->m_openError = error;
  prepared->m_preparedOpen = true;
  if (prepared->m_store) prepared->m_store->close();
  prepared->moveToThread(nullptr);
  return prepared;
}

bool DataCatalog::adoptPrepared(const std::shared_ptr<DataCatalog> &prepared, QString *error)
{
  if (!checkWriteThread("adoptPrepared", error)) return false;
  if (!prepared || !prepared->m_preparedOpen || prepared.get() == this || prepared->thread() ||
      prepared->m_lockedReadOnly != m_lockedReadOnly) {
    setError(error, QStringLiteral("invalid prepared catalog or access mode"));
    return false;
  }
  prepared->m_preparedOpen = false;
  m_store.reset(); // old connection belongs to this owner thread
  m_store = std::move(prepared->m_store);
  m_dir = prepared->m_dir;
  m_isOpen = prepared->m_isOpen;
  m_openError = prepared->m_openError;
  m_recoveredFromBackup = prepared->m_recoveredFromBackup;
  m_backupRecoveryReason = prepared->m_backupRecoveryReason;
  m_primaryCorruptOnDisk = prepared->m_primaryCorruptOnDisk;
  m_forceFullSave = prepared->m_forceFullSave;
  m_revision = prepared->m_revision;
  m_mutationSeq = qMax(m_mutationSeq + 1, prepared->m_mutationSeq);
  m_entities = std::move(prepared->m_entities);
  m_assets = std::move(prepared->m_assets);
  m_versions = std::move(prepared->m_versions);
  m_links = std::move(prepared->m_links);
  m_assetSeq = prepared->m_assetSeq; m_versionSeq = prepared->m_versionSeq;
  m_roles = std::move(prepared->m_roles);
  m_idx = std::move(prepared->m_idx);
  m_backupKeep = prepared->m_backupKeep;
  m_batchDepth = 0; m_batchDirty = false; m_batchAborted = false; m_batchSnapshot.reset();
  m_dirtyEntities.clear(); m_dirtyAssets.clear(); m_dirtyVersions.clear(); m_dirtyLinkOrds.clear();
  m_removedAssets.clear(); m_removedVersions.clear(); m_linksFullRewrite = false;
  if (m_isOpen && !m_primaryCorruptOnDisk && m_store && !m_store->resumeConnection(m_lockedReadOnly, m_revision, error)) {
    m_isOpen = false;
    m_openError = error ? *error : QStringLiteral("catalog connection could not be resumed");
    m_entities.clear(); m_assets.clear(); m_versions.clear(); m_links.clear(); m_idx.clear();
    m_store->close();
  }
  if (!m_isOpen) { setError(error, m_openError); return false; }
  if (m_recoveredFromBackup) emit backupRecovered(m_backupRecoveryReason);
  emit changed();
  return true;
}

bool DataCatalog::open(const QString &projectDir, QString *error)
{
  if (CatalogPurgeLease::isHeld(QDir(projectDir).absoluteFilePath(QStringLiteral("artifacts/metadata/catalog.json")))) {
    setError(error, tr("文件回收提交中，请等待回收完成后再打开工程。"));
    return false;
  }
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
  if (m_batchDepth == 0) m_batchAborted = false;
  ++m_batchDepth;
}

bool DataCatalog::endBatch(QString *error)
{
  if (m_batchDepth <= 0)
    return true; // 配对失衡由调用方栈结构保证，不 noisy
  if (--m_batchDepth > 0)
    return true; // 嵌套批次：只有最外层结算
  std::unique_ptr<BatchSnapshot> snap = std::move(m_batchSnapshot);
  const bool aborted = m_batchAborted;
  m_batchAborted = false;
  if (!m_batchDirty && !aborted)
    return true;
  if (!aborted && save(error)) // 成功时 commitStore 才清 m_batchDirty
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

void DataCatalog::BatchSave::abort()
{
  if (m_done || !m_catalog) return;
  m_catalog->m_batchAborted = true;
  flush();
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
