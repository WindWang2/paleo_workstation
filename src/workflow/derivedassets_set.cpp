// 层：功能
#include "derivedassets.h"
#include "evidencerecord.h"
#include "../catalog/datacatalog.h"
#include "../domain/predictionevidence.h"
#include "../services/algorithmspec.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QThread>

QVariantMap DerivedOutputSetReceipt::toVariantMap() const {
  QVariantList outputs;
  for (const auto &member : members)
    outputs.append(QVariantMap{{"portId",member.portId},{"assetId",member.assetId},
        {"versionId",member.versionId},{"path",member.relativePath},{"sha256",member.sha256}});
  return {{"schema",1},{"id",id},{"algorithmId",algorithmId},{"ready",ready},
          {"code",code},{"error",error},{"members",outputs}};
}

namespace {
bool validManagedTarget(const QString &root, const DerivedStaging &st) {
  if (!DataCatalog::isSafePathSegment(st.assetId) || !DataCatalog::isSafePathSegment(st.versionId)) return false;
  const auto file=QFileInfo(st.absolutePath);
  const auto expected=QStringLiteral("artifacts/")+DataCatalog::managedPath("derived",st.assetId,st.versionId,file.fileName());
  const auto absolute=QDir(root).absoluteFilePath(expected);
  return !file.isSymLink() && st.relativePath==expected && QDir::cleanPath(st.absolutePath)==QDir::cleanPath(absolute) &&
      file.canonicalPath()==QFileInfo(absolute).canonicalPath() &&
      file.canonicalFilePath().startsWith(QDir(root).canonicalPath()+QLatin1Char('/'));
}
QString evidenceError(const QVariantMap &extra) {
  if (extra.value("evidence").typeId()!=QMetaType::QVariantMap) return QStringLiteral("派生成果证据字段格式无效");
  for (const auto &finding : paleo::evidence::validate(extra.value("evidence").toMap()))
    if (finding.toMap().value("severity").toString()==QLatin1String("Blocking"))
      return finding.toMap().value("message").toString();
  return {};
}
}

bool DerivedAssetRegistrar::commitSet(const AlgorithmSpec &spec,const DerivedPreparedSet &prepared,
    const QStringList &parentVersionIds, const QString &sourceUri,
    const QVariantMap &extra, DerivedOutputSetReceipt &receipt) {
  receipt={};receipt.id=prepared.id;receipt.algorithmId=prepared.algorithmId;
  const auto fail=[&](const QString &code,const QString &error){receipt.ready=false;receipt.members.clear();receipt.code=code;receipt.error=error;return false;};
  if (!m_catalog || !m_catalog->isOpen() || QThread::currentThread()!=m_catalog->thread())
    return fail("catalog_unavailable",QStringLiteral("输出集登记须使用当前线程的已打开目录"));
  if (m_catalog->refusesWrites()) return fail("read_only",QStringLiteral("目录为只读，输出集未发布"));
  if (m_catalog->isStaging()) return fail("staging_catalog",QStringLiteral("内存试算目录不能发布持久输出集"));
  if (m_catalog->hasActiveBatch()) return fail("batch_in_progress",QStringLiteral("输出集必须拥有独立提交事务"));
  if (!spec.complete || spec.id!=prepared.algorithmId || prepared.id.isEmpty() || prepared.algorithmId.isEmpty() || prepared.members.isEmpty() || prepared.members.size()>64)
    return fail("invalid_output_set",QStringLiteral("输出集身份、算法或成员数量无效"));
  for (const auto &id : parentVersionIds)
    if (id.isEmpty() || m_catalog->versionById(id).id!=id)
      return fail("input_version_missing",QStringLiteral("输出集输入版本不存在：%1").arg(id));
  QSet<QString> ports,assets,versions,paths;
  QSet<QString> knownPorts,requiredPorts;
  for (const auto &port : spec.outputs) {
    if (port.kind!=QLatin1String("raster") && port.kind!=QLatin1String("vector") &&
        port.kind!=QLatin1String("table") && port.kind!=QLatin1String("file")) continue;
    if (port.id.isEmpty() || knownPorts.contains(port.id)) return fail("invalid_output_spec",QStringLiteral("输出规格端口重复/无效"));
    knownPorts.insert(port.id);if(port.required)requiredPorts.insert(port.id);
  }
  QVector<CatalogVersion> pending;
  for (const auto &member : prepared.members) {
    const auto &st=member.staging;
    if (!knownPorts.contains(member.portId) || ports.contains(member.portId) || assets.contains(st.assetId) ||
        versions.contains(st.versionId) || paths.contains(st.absolutePath) || !validManagedTarget(m_projectDir,st))
      return fail("invalid_output_target",QStringLiteral("输出端口或受管目标重复/无效"));
    ports.insert(member.portId);assets.insert(st.assetId);versions.insert(st.versionId);paths.insert(st.absolutePath);
    if (!member.fingerprint.matches(st.absolutePath))
      return fail("output_changed",QStringLiteral("输出集文件在准备后已变化：%1").arg(member.portId));
    if (!m_catalog->versionById(st.versionId).id.isEmpty() ||
        m_catalog->currentVersion(st.assetId).id!=st.expectedVersionId)
      return fail("target_conflict",QStringLiteral("输出目标版本已变化，请重新准备"));
    const auto asset=m_catalog->assetById(st.assetId);
    if ((st.reservedType.isEmpty() && asset.id.isEmpty()) || (!st.reservedType.isEmpty() && !asset.id.isEmpty()))
      return fail("target_conflict",QStringLiteral("输出目标资产已变化，请重新准备"));
    CatalogVersion v;v.id=st.versionId;v.assetId=st.assetId;v.versionNumber=st.versionNumber;
    v.stage="DERIVED";v.managed=true;v.path=st.relativePath;v.fileName=QFileInfo(st.absolutePath).fileName();
    v.sha256=member.fingerprint.sha256;v.sourceUri=sourceUri;v.parentVersionIds=parentVersionIds;v.extra=extra;
    for (auto it=member.extra.cbegin();it!=member.extra.cend();++it) v.extra.insert(it.key(),it.value());
    if (!v.extra.contains("evidence")) v.extra.insert("evidence",paleo::workflow_detail::evidenceRecord(m_catalog,parentVersionIds,v.extra));
    const auto why=evidenceError(v.extra);
    if (!why.isEmpty()) return fail("invalid_evidence",why);
    pending.append(v);
    receipt.members.append({member.portId,st.assetId,st.versionId,st.relativePath,v.sha256});
  }
  for (const auto &port : requiredPorts)
    if (!ports.contains(port)) return fail("required_output_missing",QStringLiteral("输出集缺少必需端口，未发布：%1").arg(port));
  if (!versions.contains(prepared.id)) return fail("invalid_output_set",QStringLiteral("输出集必须以其成员版本为身份"));
  // The identical manifest is durable on every member in the same transaction.
  auto manifest=receipt.toVariantMap();manifest["ready"]=true;manifest["code"]="committed";
  DataCatalog::BatchSave transaction(m_catalog);
  QString error;
  for (qsizetype i=0;i<pending.size();++i) {
    const auto &st=prepared.members[i].staging;
    if (!st.reservedType.isEmpty()) {
      CatalogAsset a;a.id=st.assetId;a.type=st.reservedType;a.displayName=st.reservedDisplayName;a.format=st.reservedFormat;
      if (!m_catalog->addAsset(a,&error)) {transaction.abort();return fail("catalog_commit_failed",error);}
    }
    pending[i].extra.insert("outputSet",manifest);
    if (!m_catalog->addVersion(pending[i],&error)) {transaction.abort();return fail("catalog_commit_failed",error);}
  }
  if (!transaction.flush(&error)) return fail("catalog_commit_failed",error);
  for (const auto &member : prepared.members) {
    m_pending.remove(member.staging.assetId);m_reserved.remove(member.staging.assetId);
    m_reservedByName.remove(member.staging.reservedType+QLatin1Char('\x1f')+member.staging.reservedDisplayName);
    QFile::setPermissions(member.staging.absolutePath,QFileDevice::ReadOwner|QFileDevice::ReadUser|QFileDevice::ReadGroup|QFileDevice::ReadOther);
  }
  receipt.ready=true;receipt.code="committed";
  return true;
}
