// 层：功能
#pragma once

// 方向57：constraintfactorjobs.cpp 按函数族拆 TU 时的跨族共享辅助。
// 先例与纪律同 constraintworkflow_internal.h（方向20 轮4）：全部 inline
// 进 paleo::constraint_detail，避免跨 TU 重复符号（LNK2005 教训）。
// 函数体与拆分前逐字一致；dropConstraintTemp 与原 NS#4 discardTempTree
// 判据逐字相同（只清 paleo-sf- 前缀临时目录，不碰用户数据）——收编为
// 一份 discardTempTree，统一异步面 cleanup 与 contour 族共用。

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <gdal.h>     // contourFeatureCountOf（U8 等值线要素计数）
#include <ogr_api.h>

#include "../catalog/datacatalog.h"
#include "../qgis/qgislayerservice.h"
#include "constraintworkflow_internal.h"  // sameFile（declaredFactorPathMatches 用）

namespace paleo::constraint_detail
{
  // SHA 只比较输入字节；报错、代次/声明守卫与清理仍留在各发布路径，保持顺序。
  inline bool analysisShaMatches( const QString &path, const QString &expected, QString *error )
  {
    return DataCatalog::sha256FileHex( path, error ) == expected;
  }

  inline void inheritMockFlag( DataCatalog *catalog, const QStringList &parents, QVariantMap &extra )
  {
    if ( !catalog )
      return;
    for ( const QString &id : parents )
      if ( catalog->versionById( id ).extra.value( QStringLiteral( "mock" ) ).toBool() )
        extra.insert( QStringLiteral( "mock" ), true );
  }

  // 方向67：策略包 id → 血缘 extra。id 已由 generateFactor 入口按
  // singlefactorstrategy 词表校验（词表外拒绝，不回退），这里只落血缘。
  inline void insertStrategyId( const QVariantMap &params, QVariantMap &extra )
  {
    QVariantMap effective = params;
    for (const auto &key : {QStringLiteral("OUTPUT"), QStringLiteral("output"),
                           QStringLiteral("parentVersionIds"), QStringLiteral("pointsLayerId"),
                           QStringLiteral("factorInputPrepared")}) effective.remove(key);
    extra.insert(QStringLiteral("algorithm_parameters"), effective);
    if (params.contains(QStringLiteral("extraction")))
      extra.insert(QStringLiteral("extraction"), params.value(QStringLiteral("extraction")));
    const QString strategyId = params.value( QStringLiteral( "strategy_id" ) ).toString();
    if ( !strategyId.isEmpty() )
      extra.insert( QStringLiteral( "strategy_id" ), strategyId );
  }

  inline QVariantMap contourMetadata( const QString &layerId, const QString &horizon,
                               const QString &factorLayerId )
  {
    QVariantMap extra;
    extra.insert( QStringLiteral( "mapping_product" ), true );
    extra.insert( QStringLiteral( "layer_id" ), layerId );
    extra.insert( QStringLiteral( "horizon" ), horizon );
    extra.insert( QStringLiteral( "layer_type" ), QStringLiteral( "vector" ) );
    extra.insert( QStringLiteral( "source_suffix" ), QStringLiteral( "|layername=contours" ) );
    extra.insert( QStringLiteral( "factor_layer_id" ), factorLayerId );
    return extra;
  }

  // 同步/异步制图工作场的相同 QC 指纹输入。层位/因素/父版本由调用方补齐。
  inline QVariantMap cartographicHashParameters( const QVariantMap &qc, const QVariantList &levels,
                                         const QString &analysisSha )
  {
    QVariantMap params;
    params.insert( QStringLiteral( "algorithm_id" ), QStringLiteral( "paleo:paleo_cartographic_work" ) );
    params.insert( QStringLiteral( "value_source" ), QStringLiteral( "cartographic_work" ) );
    params.insert( QStringLiteral( "levels" ), qc.contains( QStringLiteral( "levels" ) )
                                               ? qc.value( QStringLiteral( "levels" ) ) : QVariant( levels ) );
    params.insert( QStringLiteral( "transition_distance" ), qc.value( QStringLiteral( "transition_distance" ) ) );
    params.insert( QStringLiteral( "ignored" ), qc.value( QStringLiteral( "ignored" ) ) );
    params.insert( QStringLiteral( "used_constraints" ), qc.value( QStringLiteral( "used_constraints" ) ) );
    params.insert( QStringLiteral( "analysis_sha256" ), analysisSha );
    return params;
  }

  // 原壳（paleomainwindow runIdwRequested 接线）里的井点图层查找，挪进
  // workflow 侧：vector 声明、layerId 以 "wells" 开头，优先当前层位，其次
  // 层位无关（horizon-agnostic）声明。
  inline QString wellsLayerIdFor( QgisLayerService *layers, const QString &horizon )
  {
    if ( !layers )
      return QString();
    QVector<LayerDeclaration> declared;
    if ( !layers->tryDeclared( &declared ) )
      return QString();
    QString agnostic;
    for ( const LayerDeclaration &d : declared )
    {
      if ( d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) != 0 )
        continue;
      if ( !d.layerId.startsWith( QStringLiteral( "wells" ) ) )
        continue;
      if ( !horizon.isEmpty() && d.horizon == horizon )
        return d.layerId;
      if ( agnostic.isEmpty() && d.horizon.isEmpty() )
        agnostic = d.layerId;
    }
    return agnostic;
  }

inline void discardTempTree( const QString &path )
{
  if ( path.contains( QStringLiteral( "paleo-sf-" ) ) )
    QDir( QFileInfo( path ).absolutePath() ).removeRecursively();
}

// U8（S5 收编）：统计落盘等值线 GPKG 的 contours 图层要素数（publish 线程
// 调用，临时 GPKG 已由 compute 落盘并核验存在，开销可忽略）。打开失败或
// 无该图层返回 -1 哨兵——计数是血缘如实在案，不改变发布成功语义；
// 读侧只对 ==0 显示「0 条等值线」，-1 不误报。
inline int contourFeatureCountOf( const QString &gpkgPath )
{
  GDALDatasetH ds = GDALOpenEx( gpkgPath.toUtf8().constData(),
                                GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr );
  if ( !ds )
    return -1;
  OGRLayerH layer = GDALDatasetGetLayerByName( ds, "contours" );
  const int count = layer ? static_cast<int>( OGR_L_GetFeatureCount( layer, TRUE ) ) : -1;
  GDALClose( ds );
  return count;
}

inline QString factorIdOf( const QString &horizon, const QString &factorLayerId )
{
  const QString factorPrefix = QStringLiteral( "factor.%1." ).arg( horizon );
  if ( factorLayerId.startsWith( factorPrefix ) )
    return factorLayerId.mid( factorPrefix.size() );
  return factorLayerId;
}
inline bool resolveDeclaredRaster( QgisLayerService *layers, const QString &layerId, QString *path, QString *error )
{
  QVector<LayerDeclaration> declared;
  QString readErr;
  if ( !layers->tryDeclared( &declared, &readErr ) )
  {
    if ( error )
      *error = readErr.isEmpty() ? QObject::tr( "无法读取图层清单" ) : readErr;
    return false;
  }
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId != layerId )
      continue;
    if ( d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) != 0 )
    {
      if ( error )
        *error = QObject::tr( "等值线输入必须是栅格图层：%1" ).arg( layerId );
      return false;
    }
    *path = d.source.section( QLatin1Char( '|' ), 0, 0 );
    return true;
  }
  if ( error )
    *error = QObject::tr( "图层 %1 未在清单声明" ).arg( layerId );
  return false;
}

inline bool declaredFactorPathMatches( QgisLayerService *layers, const QString &factorLayerId, const QString &rasterPath )
{
  if ( !layers || factorLayerId.isEmpty() || rasterPath.isEmpty() )
    return false;
  QString live;
  if ( !resolveDeclaredRaster( layers, factorLayerId, &live, nullptr ) )
    return false;
  return sameFile( live, rasterPath );
}

// P1-A/P2：引擎回执（Processing 输出 CONSTRAINT_RECEIPTS，JSON 数组）→ 血缘
// extra 逐条 constraints_consumed {id,role} / constraints_unused {id,reason}。
// 结果里没有该键（旧引擎/降级路径）时不写，不臆造。
inline void insertConstraintReceiptExtras( const QVariantMap &results, QVariantMap &extra )
{
  if ( !results.contains( QStringLiteral( "CONSTRAINT_RECEIPTS" ) ) )
    return;
  QVariantList consumed, unused;
  const QJsonArray receipts =
      QJsonDocument::fromJson( results.value( QStringLiteral( "CONSTRAINT_RECEIPTS" ) ).toString().toUtf8() ).array();
  for ( const QJsonValue &value : receipts )
  {
    const QJsonObject item = value.toObject();
    if ( item.value( QStringLiteral( "consumed" ) ).toBool() )
      consumed << QVariantMap{ { QStringLiteral( "id" ), item.value( QStringLiteral( "id" ) ).toString() },
                               { QStringLiteral( "role" ), item.value( QStringLiteral( "role" ) ).toString() } };
    else
      unused << QVariantMap{ { QStringLiteral( "id" ), item.value( QStringLiteral( "id" ) ).toString() },
                             { QStringLiteral( "reason" ), item.value( QStringLiteral( "reason" ) ).toString() } };
  }
  extra.insert( QStringLiteral( "constraints_consumed" ), consumed );
  extra.insert( QStringLiteral( "constraints_unused" ), unused );
}

// P3 任务G：QC 里的 constraint_receipts（结构面等以 QC 旁路文件回执的引擎）
// → 血缘 constraints_consumed / constraints_unused，与上面同口径。无该键不写。
inline void insertQcConstraintReceiptExtras( const QVariantMap &qc, QVariantMap &extra )
{
  if ( !qc.contains( QStringLiteral( "constraint_receipts" ) ) )
    return;
  QVariantList consumed, unused;
  for ( const QVariant &value : qc.value( QStringLiteral( "constraint_receipts" ) ).toList() )
  {
    const QVariantMap item = value.toMap();
    if ( item.value( QStringLiteral( "consumed" ) ).toBool() )
      consumed << QVariantMap{ { QStringLiteral( "id" ), item.value( QStringLiteral( "id" ) ) },
                               { QStringLiteral( "role" ), item.value( QStringLiteral( "role" ) ) } };
    else
      unused << QVariantMap{ { QStringLiteral( "id" ), item.value( QStringLiteral( "id" ) ) },
                             { QStringLiteral( "reason" ), item.value( QStringLiteral( "reason" ) ) } };
  }
  extra.insert( QStringLiteral( "constraints_consumed" ), consumed );
  extra.insert( QStringLiteral( "constraints_unused" ), unused );
}

// P2 任务D：引擎逐点取样回执（SAMPLE_RECEIPTS）→ 血缘 engine_samples_used /
// engine_samples_skipped。与 supporting_wells/skipped_wells（证据记录按取数步骤
// 的井表填写，含提取阶段就被拒的井）分开：引擎只看得到进入它的那份快照，
// 两层都如实保留，不互相覆盖。没有该输出的引擎不写，不臆造。
inline void insertSampleReceiptExtras( const QVariantMap &results, QVariantMap &extra )
{
  if ( !results.contains( QStringLiteral( "SAMPLE_RECEIPTS" ) ) )
    return;
  QVariantList used, skipped;
  const QJsonArray receipts =
      QJsonDocument::fromJson( results.value( QStringLiteral( "SAMPLE_RECEIPTS" ) ).toString().toUtf8() ).array();
  for ( const QJsonValue &value : receipts )
  {
    const QVariantMap item = value.toObject().toVariantMap();
    ( item.value( QStringLiteral( "participating" ) ).toBool() ? used : skipped ) << item;
  }
  extra.insert( QStringLiteral( "engine_samples_used" ), used );
  extra.insert( QStringLiteral( "engine_samples_skipped" ), skipped );
}

} // namespace paleo::constraint_detail
