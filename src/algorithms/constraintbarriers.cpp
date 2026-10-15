// 层：数据
#include "constraintbarriers.h"

#include "domain/constraintsemantics.h"

#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfeaturesource.h>
#include <qgswkbtypes.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointF>

namespace paleo::algorithms
{

UnifiedBarrierRead readUnifiedHardBarriers( QgsFeatureSource *source, const QString &methodId,
                                            const std::function<void( QgsGeometry & )> &transform )
{
  namespace sf = paleo::singlefactor;
  UnifiedBarrierRead out;
  if ( !source )
    return out;
  const QgsFields fields = source->fields();
  const int typeIdx = fields.lookupField( QStringLiteral( "type" ) );
  const int jsonIdx = fields.lookupField( QStringLiteral( "params_json" ) );
  const int schemaIdx = fields.lookupField( QStringLiteral( "schema_version" ) );
  const int idIdx = fields.lookupField( QStringLiteral( "id" ) );
  sf::ConstraintMethodContext context;
  context.methodId = methodId;
  QgsFeatureIterator it = source->getFeatures( QgsFeatureRequest() );
  QgsFeature feature;
  while ( it.nextFeature( feature ) )
  {
    // 与 constraintparse 同一 id 口径（id 列去空白；空则 fid）。
    const QString idText = idIdx >= 0 ? feature.attribute( idIdx ).toString().trimmed() : QString();
    const QString featureId = idText.isEmpty() ? QString::number( feature.id() ) : idText;
    const QString type = typeIdx >= 0 ? feature.attribute( typeIdx ).toString().trimmed() : QString();
    QString json;
    if ( jsonIdx >= 0 && !feature.attribute( jsonIdx ).isNull() )
      json = feature.attribute( jsonIdx ).toString();
    const sf::ConstraintSemantics semantics = sf::resolveConstraintSemantics(
        type, json, schemaIdx >= 0 ? feature.attribute( schemaIdx ) : QVariant(), sf::semanticsOptionsForMethod( methodId ),
        featureId );
    if ( !feature.hasGeometry() || feature.geometry().isEmpty() )
    {
      out.receipts << sf::decideNonLineConstraint( featureId, QString(), type, semantics, true, context ).toVariant();
      continue;
    }
    QgsGeometry geometry = feature.geometry();
    if ( transform )
      transform( geometry );
    if ( QgsWkbTypes::geometryType( geometry.wkbType() ) != Qgis::GeometryType::Line )
    {
      out.receipts << sf::decideNonLineConstraint( featureId, QString(), type, semantics, false, context ).toVariant();
      continue;
    }
    const QgsMultiPolylineXY parts =
        geometry.isMultipart() ? geometry.asMultiPolyline() : QgsMultiPolylineXY{ geometry.asPolyline() };
    for ( int k = 0; k < parts.size(); ++k )
    {
      QVector<QPointF> vertices;
      vertices.reserve( parts.at( k ).size() );
      for ( const QgsPointXY &point : parts.at( k ) )
        vertices.append( point.toQPointF() );
      const sf::ConstraintUseDecision decision = sf::decideConstraintPart(
          featureId, QString(), type, semantics, vertices, k, static_cast<int>( parts.size() ), context );
      out.receipts << decision.toVariant();
      if ( decision.consumed && decision.role == QLatin1String( "hard_barrier" ) )
        out.parts << QgsGeometry::fromPolylineXY( parts.at( k ) );
    }
  }
  return out;
}

QString constraintReceiptsJson( const QVariantList &receipts )
{
  return QString::fromUtf8(
      QJsonDocument( QJsonArray::fromVariantList( receipts ) ).toJson( QJsonDocument::Compact ) );
}

} // namespace paleo::algorithms
