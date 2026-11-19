// 层：功能
#include "factorevidenceconfirmation.h"

#include "analysisresultsync.h"
#include "catalog/datacatalog.h"
#include "paleo_data/service.h"
#include "services/subintervalfactors.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantMap>

namespace paleo::factor_evidence
{

namespace
{

// 与 syncAnalysisResults 生成的绑定 id 同口径：catalog.<versionId>。
QString expectedBindingId( const QString &versionId ) { return QStringLiteral( "catalog." ) + versionId; }

bool bindingExists( const QString &projectDir, const paleo::strat::UnitSelection &unit,
                    const QString &versionId, QString *reason )
{
  paleo::data::DataService ds;
  QString openError;
  if( !paleo::strat::openProjectDataService( projectDir, ds, &openError ) )
  {
    if( reason )
      *reason = openError.isEmpty() ? QStringLiteral( "Data Service 不可用" ) : openError;
    return false;
  }
  std::vector<paleo::data::SubIntervalResultBinding> bindings;
  std::string detail;
  if( !ds.listSubIntervalResults( unit.schemeId.toStdString(),
                                  unit.subIntervalId.toStdString(), bindings, detail ) )
  {
    if( reason )
      *reason = QString::fromStdString( detail.empty() ? QStringLiteral( "绑定查询失败" ).toStdString() : detail );
    return false;
  }
  const QString id = expectedBindingId( versionId );
  for( const auto &b : bindings )
    if( b.id == id.toStdString() && b.assetRef == versionId.toStdString() )
      return true;
  if( reason )
    *reason = QStringLiteral( "目标绑定 %1 尚未登记" ).arg( id );
  return false;
}

bool writePendingFlag( DataCatalog *catalog, const QString &versionId, const QVariantMap &pending )
{
  QString error;
  if( pending.isEmpty() )
    return catalog->updateVersionExtra( versionId, QStringLiteral( "evidence_pending" ), QVariant(), &error );
  return catalog->updateVersionExtra( versionId, QStringLiteral( "evidence_pending" ), pending, &error );
}

} // namespace

Confirmation confirmVersionEvidence( DataCatalog *catalog, const QString &projectDir,
                                     const QString &versionId, const paleo::strat::UnitSelection &unit )
{
  Confirmation out;
  out.versionId = versionId;
  out.bindingId = expectedBindingId( versionId );
  out.catalogCommitted = !versionId.isEmpty() && catalog && catalog->versionById( versionId ).id == versionId;
  if( !out.catalogCommitted || !unit.isValid() || unit.schemeId.isEmpty() )
  {
    out.pendingReason = !out.catalogCommitted ? QStringLiteral( "受管版本不存在：%1" ).arg( versionId )
                                              : QStringLiteral( "冻结单元身份无效，无法登记证据" );
    return out;
  }
  // 先跑既有幂等投影（按绑定 id 去重、stale 传播），再核验目标绑定——完成
  // 判定只认绑定存在，不把同步函数返回空字符串当作全部确认。
  const QString syncError = syncAnalysisResults( catalog, projectDir );
  QString reason;
  if( syncError.isEmpty() && bindingExists( projectDir, unit, versionId, &reason ) )
  {
    out.evidenceConfirmed = true;
    const auto version = catalog->versionById( versionId );
    if( version.extra.contains( QStringLiteral( "evidence_pending" ) ) )
      writePendingFlag( catalog, versionId, {} );
    return out;
  }
  out.pendingReason = !syncError.isEmpty() ? syncError : reason;
  QVariantMap pending;
  pending.insert( QStringLiteral( "step" ), QStringLiteral( "ds_binding" ) );
  pending.insert( QStringLiteral( "error" ), out.pendingReason );
  pending.insert( QStringLiteral( "bindingId" ), out.bindingId );
  pending.insert( QStringLiteral( "unitKey" ),
                  paleo::strat::analysisLayerKey( unit, unit.subIntervalId ) );
  // 只读目录写不进标记：状态仍如实返回（绑定查询是权威事实），恢复重试兜底。
  writePendingFlag( catalog, versionId, pending );
  return out;
}

QStringList pendingEvidenceVersions( DataCatalog *catalog )
{
  QStringList out;
  if( !catalog )
    return out;
  for( const auto &v : catalog->versions() )
    if( v.extra.contains( QStringLiteral( "evidence_pending" ) ) )
      out.append( v.id );
  return out;
}

int retryPendingEvidence( DataCatalog *catalog, const QString &projectDir, QString *error )
{
  if( !catalog )
  {
    if( error )
      *error = QStringLiteral( "catalog 不可用" );
    return -1;
  }
  const QStringList pending = pendingEvidenceVersions( catalog );
  int remaining = 0;
  QString firstError;
  for( const auto &versionId : pending )
  {
    const auto version = catalog->versionById( versionId );
    if( version.id.isEmpty() )
      continue;
    const auto unit = paleo::strat::fromMap( version.extra.value( QStringLiteral( "unitSelection" ) ).toMap() );
    if( !unit.isValid() )
    {
      // 无冻结单元的历史产物不能补登记（不虚构身份）；标记保留并如实计数。
      ++remaining;
      continue;
    }
    const auto result = confirmVersionEvidence( catalog, projectDir, versionId, unit );
    if( !result.evidenceConfirmed )
    {
      ++remaining;
      if( firstError.isEmpty() )
        firstError = result.pendingReason;
    }
  }
  if( error && remaining > 0 )
    *error = firstError;
  return remaining;
}

} // namespace paleo::factor_evidence
