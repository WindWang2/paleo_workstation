// 层：功能
#include "workflows.h"
#include "workflows_internal.h"

#include "../domain/arearules.h"        // AreaRules::active（工程配置门）
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"   // LayerDeclaration / QgisLayerService
#include "../services/projectdata.h"
#include "mappingworkflow.h"          // TimeResidualRow / computeTimeResiduals

#include <QFile>
#include <QVector>

#include "../qgis/qgislayerservice.h"   // LayerDeclaration / QgisLayerService
#include "../metadata/paleoprojectstore.h"
#include "../services/projectdata.h"

#include <QVector>

// 验证工作流（④）。
// 方向20 轮4：按类边界从 workflows.cpp 析出（本段零共享内部辅助依赖，
// 故无需共享头）。头文件契约不动。


// ---------------------------------------------------------------------------
// ValidationWorkflow — ④验证
// ---------------------------------------------------------------------------

ValidationWorkflow::ValidationWorkflow( QgisLayerService *layers, PaleoProjectStore *store, QObject *parent )
  : QObject( parent ), m_layers( layers ), m_store( store )
{
}

void ValidationWorkflow::setProjectData( ProjectDataFacade *projectData )
{
  m_projectData = projectData;
}

ProjectDataFacade *ValidationWorkflow::projectData() const
{
  return m_projectData.data();
}

void ValidationWorkflow::setResidualThresholdMs( double thresholdMs )
{
  m_residualThresholdMs = thresholdMs;
}

QVariantList ValidationWorkflow::lastResidualRows() const
{
  return m_residualRows;
}

QgisLayerService *ValidationWorkflow::layerService() const
{
  return m_layers.data();
}

PaleoProjectStore *ValidationWorkflow::projectStore() const
{
  return m_store.data();
}

QList<ValidationIssue> ValidationWorkflow::validate()
{
  QList<ValidationIssue> issues;
  QgisLayerService *layers = m_layers.data();
  PaleoProjectStore *store = m_store.data();
  QVector<LayerDeclaration> decls;
  if ( layers )
  {
    QString manifestErr;
    if ( !layers->tryDeclared( &decls, &manifestErr ) )
    {
      // Manifest unreadable is a finding, not a clean bill — otherwise a
      // corrupt store validates as "no issues".
      ValidationIssue v;
      v.severity = ValidationIssue::Error;
      v.code = QStringLiteral( "MANIFEST_READ_FAILED" );
      v.message = manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr;
      issues.append( v );
    }
  }

  // (a) declared raster/vector layers whose file source is missing on disk.
  for ( const LayerDeclaration &d : decls )
  {
    const bool fileType = d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) == 0 ||
                          d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) == 0;
    if ( !fileType || !paleo::workflow_detail::isFileBackedSource( d.source ) )
      continue;
    const QString base = d.source.section( QLatin1Char( '|' ), 0, 0 );
    if ( !QFile::exists( base ) )
    {
      ValidationIssue v;
      v.severity = ValidationIssue::Error;
      v.code = QStringLiteral( "SRC_MISSING" );
      v.message = tr( "已声明图层 %1 的源文件在磁盘上不存在：%2" ).arg( d.layerId, base );
      v.layerId = d.layerId;
      v.horizon = d.horizon;
      issues.append( v );
    }
  }

  // (b) duplicate horizon names. The manifest's horizons() set is DISTINCT, so
  // an exact duplicate cannot occur; what slips through is a collision after
  // normalization (case / stray whitespace) — e.g. "T1" vs "t1".
  {
    QSet<QString> raw;
    for ( const LayerDeclaration &d : decls )
      if ( !d.horizon.isEmpty() )
        raw.insert( d.horizon );

    QHash<QString, QStringList> byNormalized;
    for ( const QString &h : raw )
      byNormalized[h.trimmed().toLower()].append( h );

    for ( auto it = byNormalized.constBegin(); it != byNormalized.constEnd(); ++it )
    {
      if ( it.value().size() < 2 )
        continue;
      QStringList names = it.value();
      names.sort();
      ValidationIssue v;
      v.severity = ValidationIssue::Warning;
      v.code = QStringLiteral( "DUP_HORIZON" );
      v.message = tr( "层位名冲突（归一化后相同）：%1" ).arg( names.join( QStringLiteral( " / " ) ) );
      v.horizon = names.first();
      issues.append( v );
    }
  }

  // (c) busy-layer conflicts — any declared layer still held by a running task.
  if ( store )
  {
    for ( const LayerDeclaration &d : decls )
    {
      QString reason;
      if ( store->layerBusy( d.layerId, &reason ) )
      {
        ValidationIssue v;
        v.severity = ValidationIssue::Info;
        v.code = QStringLiteral( "BUSY" );
        v.message = tr( "图层 %1 正忙：%2" ).arg( d.layerId, reason );
        v.layerId = d.layerId;
        v.horizon = d.horizon;
        issues.append( v );
      }
    }
  }

  // (d) 井上标定层位时间残差（autoplan §5C）— 只评 targetHorizon（工程参数，
  // 本工区 D61）。每口井一行（验证页残差表渲染源，存 paleo.wf.residualRows）；
  // |r|>阈值 → TIME_RESIDUAL 问题。
  // 默认阈值 10 ms；非数值行（无分层/TD 原因/不在测网内/落在空道）不成问题。
  {
    const QString tgtHorizon = AreaRules::active().targetHorizon;
    QVariantList rowMaps;
    if ( auto *pd = m_projectData.data() )
    {
      const double prop = m_residualThresholdMs;
      const double threshold = prop > 0.0 ? prop : 10.0;
      const QList<TimeResidualRow> rows =
          computeTimeResiduals( pd, tgtHorizon, threshold );
      // 残差行所属栅格图层（问题行/残差行的地图缩放目标与联动 layerId）。
      const HorizonRasterInfo rasterInfo =
          pd->horizonRasterDecl( tgtHorizon );
      const QString rasterLayerId = rasterInfo.layerId;
      // T25：有井但一行残差都没有 → 栅格没声明或打不开，残差检查其实没跑成；
      // 发 RASTER_MISSING 让问题表/摘要行说清原因，不和「还没计算」混为一谈。
      // 声明在而文件缺/打不开时复述底座原因（lastError），不写「还没有」。
      if ( rows.isEmpty() && !pd->wells().isEmpty() )
      {
        ValidationIssue v;
        v.severity = ValidationIssue::Warning;
        v.code = QStringLiteral( "RASTER_MISSING" );
        v.message = rasterLayerId.isEmpty()
                        ? tr( "层位 %1 还没有时间栅格" ).arg( tgtHorizon )
                        : ( pd->lastError().isEmpty()
                                ? tr( "层位 %1 的时间栅格不可用" ).arg( tgtHorizon )
                                : pd->lastError() );
        v.horizon = tgtHorizon;
        v.layerId = rasterLayerId;
        issues.append( v );
      }
      for ( const TimeResidualRow &row : rows )
      {
        QVariantMap m;
        m.insert( QStringLiteral( "well_id" ), row.wellId );
        m.insert( QStringLiteral( "well_name" ), row.wellName );
        m.insert( QStringLiteral( "horizon" ), tgtHorizon );
        m.insert( QStringLiteral( "layer_id" ), rasterLayerId );
        m.insert( QStringLiteral( "threshold_ms" ), threshold );
        m.insert( QStringLiteral( "reason" ), row.reason );
        if ( std::isfinite( row.x ) )
          m.insert( QStringLiteral( "x" ), row.x );
        if ( std::isfinite( row.y ) )
          m.insert( QStringLiteral( "y" ), row.y );
        if ( std::isfinite( row.timeMs ) )
          m.insert( QStringLiteral( "time_ms" ), row.timeMs );
        if ( std::isfinite( row.rasterMs ) )
          m.insert( QStringLiteral( "raster_ms" ), row.rasterMs );
        if ( std::isfinite( row.residualMs ) )
          m.insert( QStringLiteral( "residual_ms" ), row.residualMs );
        m.insert( QStringLiteral( "inline" ), row.inlineNo );
        const char *status = row.status == TimeResidualRow::Status::Pass       ? "pass"
                             : row.status == TimeResidualRow::Status::Exceeds  ? "exceed"
                             : row.status == TimeResidualRow::Status::Warn     ? "warn"
                                                                               : "na";
        m.insert( QStringLiteral( "status" ), QString::fromLatin1( status ) );
        rowMaps.append( m );

        if ( row.status != TimeResidualRow::Status::Exceeds )
          continue;
        ValidationIssue v;
        v.severity = ValidationIssue::Warning;
        v.code = QStringLiteral( "TIME_RESIDUAL" );
        QString inlineText;
        if ( row.inlineNo >= 0 )
          inlineText = tr( "，目标测线 %1" ).arg( row.inlineNo );
        v.message = tr( "井 %1 %2 时间残差 %3ms（井 %4ms vs 栅格 %5ms%6）" )
                        .arg( row.wellName, tgtHorizon )
                        .arg( row.residualMs, 0, 'f', 1 )
                        .arg( row.timeMs, 0, 'f', 1 )
                        .arg( row.rasterMs, 0, 'f', 1 )
                        .arg( inlineText );
        v.horizon = tgtHorizon;
        v.wellId = row.wellId;
        if ( std::isfinite( row.x ) && std::isfinite( row.y ) )
          v.wktLocation = QStringLiteral( "POINT(%1 %2)" ).arg( row.x ).arg( row.y );
        v.details.insert( QStringLiteral( "well_name" ), row.wellName );
        v.details.insert( QStringLiteral( "well_x" ), row.x );
        v.details.insert( QStringLiteral( "well_y" ), row.y );
        v.details.insert( QStringLiteral( "inline" ), row.inlineNo );
        v.details.insert( QStringLiteral( "time_ms" ), row.timeMs );
        v.details.insert( QStringLiteral( "raster_ms" ), row.rasterMs );
        v.details.insert( QStringLiteral( "residual_ms" ), row.residualMs );
        v.layerId = rasterLayerId;
        issues.append( v );
      }
    }
    m_residualRows = rowMaps;
  }

  emit validationDone( issues.size() );
  return issues;
}
