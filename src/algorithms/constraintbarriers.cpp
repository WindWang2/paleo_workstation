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

#include <algorithm>
#include <cmath>

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
      {
        out.parts << QgsGeometry::fromPolylineXY( parts.at( k ) );
        out.partReceipt << out.receipts.size() - 1;
      }
    }
  }
  return out;
}

int rasterizeBarrierParts( UnifiedBarrierRead &read, double originX, double originYTop, double cellSize, int cols,
                           int rows, std::vector<std::uint8_t> &mask, const std::function<bool()> &cancelled )
{
  mask.assign( static_cast<std::size_t>( rows ) * cols, 0 );
  const auto cellOf = [&]( double x, double y, int &c, int &r ) {
    c = static_cast<int>( std::floor( ( x - originX ) / cellSize ) );
    r = static_cast<int>( std::floor( ( originYTop - y ) / cellSize ) );
    return c >= 0 && c < cols && r >= 0 && r < rows;
  };
  int touchedParts = 0;
  for ( int p = 0; p < read.parts.size(); ++p )
  {
    const QgsPolylineXY line = read.parts.at( p ).asPolyline();
    bool touched = false;
    for ( int i = 1; i < line.size(); ++i )
    {
      if ( cancelled && cancelled() )
        return -1;
      const QgsPointXY &a = line[i - 1];
      const QgsPointXY &b = line[i];
      const int steps = std::max( 1, static_cast<int>( std::ceil( a.distance( b ) / ( cellSize * 0.5 ) ) ) );
      for ( int s = 0; s <= steps; ++s )
      {
        int c = 0, r = 0;
        if ( cellOf( a.x() + ( b.x() - a.x() ) * s / steps, a.y() + ( b.y() - a.y() ) * s / steps, c, r ) )
        {
          mask[static_cast<std::size_t>( r ) * cols + c] = 1;
          touched = true;
        }
      }
    }
    if ( touched )
    {
      ++touchedParts;
      continue;
    }
    const int index = p < read.partReceipt.size() ? read.partReceipt.at( p ) : -1;
    if ( index >= 0 && index < read.receipts.size() )
    {
      QVariantMap receipt = read.receipts.at( index ).toMap();
      receipt.insert( QStringLiteral( "consumed" ), false );
      receipt.insert( QStringLiteral( "role" ), QString() );
      receipt.insert( QStringLiteral( "reason" ), QStringLiteral( "outside_compute_domain" ) );
      read.receipts[index] = receipt;
    }
  }
  return touchedParts;
}

QString constraintReceiptsJson( const QVariantList &receipts )
{
  return QString::fromUtf8(
      QJsonDocument( QJsonArray::fromVariantList( receipts ) ).toJson( QJsonDocument::Compact ) );
}

} // namespace paleo::algorithms
