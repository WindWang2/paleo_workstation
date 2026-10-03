// 层：功能
#include "constraintimport.h"

#include "../algorithms/singlefactor/wellacquisition.h"

#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>

#include <QFileInfo>
#include <QJsonDocument>
#include <QObject>

namespace paleo
{
namespace
{
namespace sf = singlefactor;

sf::FeatureAttributes featureAttributes( const QgsFeature &feature )
{
  sf::FeatureAttributes attributes;
  const QgsFields fields = feature.fields();
  const QVariantList values = feature.attributes();
  for ( int i = 0; i < fields.size() && i < values.size(); ++i )
    attributes.append( { fields.at( i ).name(), values.at( i ) } );
  return attributes;
}

bool hasField( const QgsFields &fields, const QString &name )
{
  for ( int i = 0; i < fields.size(); ++i )
  {
    if ( fields.at( i ).name().compare( name, Qt::CaseInsensitive ) == 0 )
      return true;
  }
  return false;
}

// auto 角色：字段特征优先（RATIO/INF_RADIUS/CORE_R → 方向；
// BLK_MODE/BLOCK_MODE → 打断），否则文件名提示，仍歧义报错。
QString detectRole( const QgsVectorLayer &layer, const QString &path, QString *error )
{
  const QgsFields fields = layer.fields();
  // Paleo 快照/存储格式（type + params_json 列）回导保持原状。
  if ( hasField( fields, QStringLiteral( "params_json" ) ) )
    return QStringLiteral( "generic" );
  const bool directionFields = hasField( fields, QStringLiteral( "RATIO" ) ) ||
                               hasField( fields, QStringLiteral( "INF_RADIUS" ) ) ||
                               hasField( fields, QStringLiteral( "CORE_R" ) );
  const bool barrierFields = hasField( fields, QStringLiteral( "BLK_MODE" ) ) ||
                             hasField( fields, QStringLiteral( "BLOCK_MODE" ) );
  if ( directionFields && !barrierFields )
    return QStringLiteral( "direction" );
  if ( barrierFields && !directionFields )
    return QStringLiteral( "barrier" );

  const QString name = QFileInfo( path ).fileName();
  const QString lowered = name.toLower();
  const bool directionHint = name.contains( QStringLiteral( "方向" ) ) ||
                             lowered.contains( QStringLiteral( "direction" ) );
  const bool barrierHint = name.contains( QStringLiteral( "打断" ) ) ||
                           name.contains( QStringLiteral( "断" ) ) ||
                           lowered.contains( QStringLiteral( "barrier" ) ) ||
                           lowered.contains( QStringLiteral( "break" ) );
  if ( directionHint && !barrierHint )
    return QStringLiteral( "direction" );
  if ( barrierHint && !directionHint )
    return QStringLiteral( "barrier" );
  if ( error )
    *error = QObject::tr( "无法判断约束线类型，请指定「方向线」或「打断线」" );
  return QString();
}

QVariantMap directionParams( const sf::FeatureAttributes &attrs )
{
  QVariantMap params;
  params.insert( QStringLiteral( "semantic" ), QStringLiteral( "direction_guide" ) );
  params.insert( QStringLiteral( "enabled" ),
                 sf::toBool( sf::attrValue( attrs, { QStringLiteral( "ACTIVE" ),
                                                     QStringLiteral( "active" ) } ),
                             true ) );
  // 上游 request.direction_ratio_default=8（冻结 UI 默认）。
  params.insert( QStringLiteral( "ratio" ),
                 sf::toFloat( sf::attrValue( attrs, { QStringLiteral( "RATIO" ) } ), 8.0 ) );
  params.insert( QStringLiteral( "influenceRadius" ),
                 sf::toFloat( sf::attrValue( attrs, { QStringLiteral( "INF_RADIUS" ),
                                                      QStringLiteral( "influence_radius" ) } ),
                              0.0 ) );
  params.insert( QStringLiteral( "coreRadius" ),
                 sf::toFloat( sf::attrValue( attrs, { QStringLiteral( "CORE_R" ),
                                                      QStringLiteral( "core_radius" ) } ),
                              0.0 ) );
  params.insert( QStringLiteral( "priority" ),
                 sf::toInt( sf::attrValue( attrs, { QStringLiteral( "priority" ) } ), 1 ) );
  const QVariant sourceId =
      sf::attrValue( attrs, { QStringLiteral( "ID" ), QStringLiteral( "name" ) } );
  if ( !sourceId.isNull() && sourceId.toString() != QLatin1String( "" ) )
    params.insert( QStringLiteral( "sourceId" ), sourceId.toString() );
  const QVariant zone = sf::attrValue( attrs, { QStringLiteral( "zone_id" ),
                                                QStringLiteral( "zone" ) } );
  if ( !zone.isNull() && !zone.toString().isEmpty() )
    params.insert( QStringLiteral( "zoneId" ), zone.toString() );
  const QVariant extend =
      sf::attrValue( attrs, { QStringLiteral( "extend_mode" ), QStringLiteral( "extend" ) } );
  if ( !extend.isNull() && !extend.toString().isEmpty() )
    params.insert( QStringLiteral( "extendMode" ), extend.toString() );
  const std::optional<double> transition =
      sf::parseNumeric( sf::attrValue( attrs, { QStringLiteral( "transition" ) } ) );
  if ( transition )
    params.insert( QStringLiteral( "transition" ), *transition );
  return params;
}

QVariantMap barrierParams( const sf::FeatureAttributes &attrs )
{
  QVariantMap params;
  params.insert( QStringLiteral( "semantic" ), QStringLiteral( "hard_barrier" ) );
  params.insert( QStringLiteral( "enabled" ),
                 sf::toBool( sf::attrValue( attrs, { QStringLiteral( "ACTIVE" ),
                                                     QStringLiteral( "active" ) } ),
                             true ) );
  // 上游 str(_attr_value(block_mode, blk_mode) or "full_block")——保留原文。
  QVariant mode = sf::attrValue( attrs, { QStringLiteral( "BLOCK_MODE" ),
                                          QStringLiteral( "BLK_MODE" ),
                                          QStringLiteral( "block_mode" ) } );
  QString modeText = mode.toString();
  if ( modeText.isEmpty() )
    modeText = QStringLiteral( "full_block" );
  params.insert( QStringLiteral( "blockMode" ), modeText );
  params.insert( QStringLiteral( "priority" ),
                 sf::toInt( sf::attrValue( attrs, { QStringLiteral( "priority" ) } ), 3 ) );
  const QVariant sourceId =
      sf::attrValue( attrs, { QStringLiteral( "ID" ), QStringLiteral( "name" ) } );
  if ( !sourceId.isNull() && sourceId.toString() != QLatin1String( "" ) )
    params.insert( QStringLiteral( "sourceId" ), sourceId.toString() );
  return params;
}

} // namespace

QVector<ImportedConstraintRecord> readConstraintImportFeatures( const QString &path,
                                                                const QString &role,
                                                                QString *resolvedRole,
                                                                QString *error )
{
  QgsVectorLayer layer( path, QStringLiteral( "constraints" ), QStringLiteral( "ogr" ) );
  if ( !layer.isValid() )
  {
    if ( error )
      *error = QObject::tr( "请选择有效的线矢量文件（GeoPackage / GeoJSON / Shapefile）" );
    return {};
  }
  // 局部米制坐标门在调用侧（MappingWorkbench::importConstraints）执行，
  // 便于记录级提取单测不依赖工程 CRS 词表。

  QString effective = role;
  if ( effective.isEmpty() || effective == QLatin1String( "auto" ) )
  {
    effective = detectRole( layer, path, error );
    if ( effective.isEmpty() )
      return {};
  }
  if ( effective != QLatin1String( "direction" ) && effective != QLatin1String( "barrier" ) &&
       effective != QLatin1String( "generic" ) )
  {
    if ( error )
      *error = QObject::tr( "未知约束线角色：%1" ).arg( role );
    return {};
  }
  if ( resolvedRole )
    *resolvedRole = effective;

  QVector<ImportedConstraintRecord> records;
  QgsFeatureIterator it = layer.getFeatures();
  QgsFeature feature;
  while ( it.nextFeature( feature ) )
  {
    const QgsGeometry geometry = feature.geometry();
    if ( geometry.isEmpty() || QgsWkbTypes::geometryType( geometry.wkbType() ) !=
                                   Qgis::GeometryType::Line ||
         !geometry.isGeosValid() )
    {
      if ( error )
        *error = QObject::tr( "约束文件包含无效几何，未导入" );
      return {};
    }
    const sf::FeatureAttributes attrs = featureAttributes( feature );
    QVariantMap params;
    QString type;
    if ( effective == QLatin1String( "direction" ) )
    {
      params = directionParams( attrs );
      type = QStringLiteral( "direction_line" );
    }
    else if ( effective == QLatin1String( "barrier" ) )
    {
      params = barrierParams( attrs );
      type = QStringLiteral( "break_line" );
    }
    else
    {
      // generic：Paleo 快照回导——type/params_json 两列原样回读，
      // 落库时由 lineParamsJson 做语义校验与缺省补齐。
      type = sf::attrValue( attrs, { QStringLiteral( "type" ) } ).toString();
      const QString paramsJson =
          sf::attrValue( attrs, { QStringLiteral( "params_json" ) } )
              .toString();
      if ( !paramsJson.isEmpty() )
      {
        const QJsonDocument doc =
            QJsonDocument::fromJson( paramsJson.toUtf8() );
        if ( !doc.isObject() )
        {
          if ( error )
            *error = QObject::tr( "约束文件 params_json 列不是合法 JSON" );
          return {};
        }
        params = doc.object().toVariantMap();
      }
    }
    // 上游按 feature 顺序输出；multipart 逐段拆开（保持段序）。
    const QgsMultiPolylineXY parts = geometry.isMultipart()
                                         ? geometry.asMultiPolyline()
                                         : QgsMultiPolylineXY{ geometry.asPolyline() };
    for ( const QgsPolylineXY &part : parts )
    {
      if ( part.size() < 2 )
        continue;
      QgsGeometry partGeometry = QgsGeometry::fromPolylineXY( part );
      ImportedConstraintRecord record;
      record.wkt = partGeometry.asWkt();
      record.type = type;
      record.params = params;
      records.push_back( std::move( record ) );
    }
  }
  if ( records.isEmpty() && error )
    *error = QObject::tr( "约束文件没有线要素" );
  return records;
}

} // namespace paleo
