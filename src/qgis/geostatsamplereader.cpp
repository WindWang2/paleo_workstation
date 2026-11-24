// 层：QGIS 封装
// qgis/geostatsamplereader.cpp — 见 geostatsamplereader.h。
#include "geostatsamplereader.h"
#include "algorithms/geostat/types.h" // 重头直含（transitive 闸口径）

#include <qgscoordinatereferencesystem.h>
#include <qgsfeature.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsvectorlayer.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace paleo::geostat
{
namespace
{

bool numericFieldType( QMetaType::Type type )
{
  switch( type )
  {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Short:
    case QMetaType::UShort:
      return true;
    default:
      return false;
  }
}

} // namespace

GeostatSampleReadResult readGeostatPointSamples( const QString &uri, const QString &field,
                                                  int maxSamples,
                                                  const std::function<bool()> &cancelled )
{
  GeostatSampleReadResult result;
  const auto fail = [&result]( const QString &code, const QString &message ) {
    result.ok = false;
    result.code = code;
    result.message = message;
    return result;
  };
  if( uri.isEmpty() || field.isEmpty() )
    return fail( QStringLiteral( "geostat_reader_input_invalid" ),
                 QStringLiteral( "样本 URI 或字段名为空" ) );
  if( maxSamples <= 0 )
    return fail( QStringLiteral( "geostat_reader_budget_invalid" ), QString() );

  // 调用线程自建自弃：与既有地统计任务线程的 QgsVectorLayer 用法同款，
  // 不跨线程共享图层实例。
  QgsVectorLayer layer( uri, QStringLiteral( "paleo-geostat-samples" ), QStringLiteral( "ogr" ) );
  if( !layer.isValid() )
    return fail( QStringLiteral( "geostat_reader_source_unreadable" ),
                 QStringLiteral( "样本图层无法打开（%1）" ).arg( uri ) );

  const QgsCoordinateReferenceSystem crs = layer.crs();
  if( !crs.isValid() )
    return fail( QStringLiteral( "geostat_reader_crs_missing" ),
                 QStringLiteral( "样本图层缺少坐标系" ) );
  if( crs.isGeographic() )
    return fail( QStringLiteral( "geostat_reader_crs_geographic" ),
                 QStringLiteral( "地理经纬度样本不被支持：请提供投影（米制）坐标系" ) );
  if( crs.mapUnits() != Qgis::DistanceUnit::Meters )
    return fail( QStringLiteral( "geostat_reader_crs_units_unsupported" ),
                 QStringLiteral( "样本坐标系单位非米制（当前=%1）" )
                     .arg( QString::number( static_cast< int >( crs.mapUnits() ) ) ) );

  const int fieldIndex = layer.fields().indexFromName( field );
  if( fieldIndex < 0 )
    return fail( QStringLiteral( "geostat_reader_field_missing" ),
                 QStringLiteral( "样本图层缺少数值字段 %1" ).arg( field ) );
  if( !numericFieldType( static_cast< QMetaType::Type >( layer.fields().at( fieldIndex ).type() ) ) )
    return fail( QStringLiteral( "geostat_reader_field_not_numeric" ),
                 QStringLiteral( "字段 %1 不是数值类型" ).arg( field ) );

  QgsFeature feature;
  auto iterator = layer.getFeatures();
  while( iterator.nextFeature( feature ) )
  {
    if( cancelled && cancelled() )
      return fail( QStringLiteral( "cancelled" ), QString() );
    const QgsGeometry geometry = feature.geometry();
    const QVariant value = feature.attribute( fieldIndex );
    bool numeric = false;
    const double z = value.toDouble( &numeric );
    // 缺测判定与 UI 链 sampleGeostatWells 同口径：OGR 的 NULL 数值经 QGIS 给出
    // 的是「Double、值 0、isNull()==true」的 QVariant——只看 toDouble(&ok) 会把
    // 缺测当成 0 值样本。isNull 一律按缺测跳过并计数。
    if( geometry.isNull() || geometry.isEmpty() || !value.isValid() || value.isNull() ||
        !numeric || !std::isfinite( z ) )
    {
      ++result.skippedRows;
      continue;
    }
    // 点样本契约：点/多点几何取质心（单点即自身）；线/面几何拒绝，
    // 不悄悄取首点。
    if( geometry.type() != Qgis::GeometryType::Point )
    {
      ++result.skippedRows;
      continue;
    }
    const QgsPointXY center = geometry.centroid().asPoint();
    if( !std::isfinite( center.x() ) || !std::isfinite( center.y() ) )
    {
      ++result.skippedRows;
      continue;
    }
    if( !result.hasBounds )
    {
      result.minX = result.maxX = center.x();
      result.minY = result.maxY = center.y();
      result.hasBounds = true;
    }
    else
    {
      result.minX = std::min( result.minX, center.x() );
      result.maxX = std::max( result.maxX, center.x() );
      result.minY = std::min( result.minY, center.y() );
      result.maxY = std::max( result.maxY, center.y() );
    }
    if( static_cast< int >( result.samples.size() ) >= maxSamples )
      return fail( QStringLiteral( "geostat_sample_budget_exceeded" ),
                   QStringLiteral( "样本数超过上限 %1" ).arg( maxSamples ) );
    result.samples.push_back( Sample{ center.x(), center.y(), z } );
  }
  if( result.samples.empty() )
    return fail( QStringLiteral( "geostat_reader_no_valid_samples" ),
                 QStringLiteral( "没有可用样本（跳过 %1 行）" ).arg( result.skippedRows ) );

  result.crsWkt = crs.toWkt();
  result.crsAuthId = crs.authid();
  result.crs = RasterCrs( crs );
  result.ok = true;
  return result;
}

RasterCrs geostatRasterCrsFromWkt( const QString &wkt )
{
  const QgsCoordinateReferenceSystem crs = QgsCoordinateReferenceSystem::fromWkt( wkt );
  return RasterCrs( crs );
}

} // namespace paleo::geostat
