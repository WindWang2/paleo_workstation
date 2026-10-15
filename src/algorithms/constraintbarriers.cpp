// 层：数据
#include "constraintbarriers.h"

#include "domain/constraintsemantics.h"

#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfeaturesource.h>
#include <qgswkbtypes.h>

#include <QJsonArray>
#include <QStringList>
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

void supercoverPolylineCells( const QVector<QPointF> &vertices, double originX, double originYTop,
                              double pixelWidth, double pixelHeight, int cols, int rows,
                              const std::function<void( int, int )> &visit )
{
  if ( cols <= 0 || rows <= 0 || !( pixelWidth > 0.0 ) || !( pixelHeight < 0.0 ) || !visit )
    return;
  const double gridRight = originX + static_cast<double>( cols ) * pixelWidth;
  const double gridBottom = originYTop + static_cast<double>( rows ) * pixelHeight;
  // 空间判定契约（P1，pr457-closure）：「触及计算域」与计划侧
  // polylineTouchesRect 同为闭矩形；落在右/下边界上的点 clamp 进最后一列/行
  // 格——贴边屏障影响边缘格，不是域外。
  const auto colOf = [&]( double x ) {
    return std::clamp( static_cast<int>( std::floor( ( x - originX ) / pixelWidth ) ), 0, cols - 1 );
  };
  const auto rowOf = [&]( double y ) {
    return std::clamp( static_cast<int>( std::floor( ( y - originYTop ) / pixelHeight ) ), 0, rows - 1 );
  };
  // Liang–Barsky 裁剪到闭矩形（就地收缩 a/b；返回是否与域相交）。
  const auto clipToGrid = [&]( QPointF &a, QPointF &b ) {
    double t0 = 0, t1 = 1;
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double p[4] = { -dx, dx, -dy, dy };
    const double q[4] = { a.x() - originX, gridRight - a.x(), a.y() - gridBottom, originYTop - a.y() };
    for ( int i = 0; i < 4; ++i )
    {
      if ( p[i] == 0 )
      {
        if ( q[i] < 0 )
          return false;
        continue;
      }
      const double t = q[i] / p[i];
      if ( p[i] < 0 )
        t0 = std::max( t0, t );
      else
        t1 = std::min( t1, t );
      if ( t0 > t1 )
        return false;
    }
    const QPointF originalA = a, originalB = b;
    a = QPointF( originalA.x() + ( originalB.x() - originalA.x() ) * t0,
                 originalA.y() + ( originalB.y() - originalA.y() ) * t0 );
    b = QPointF( originalA.x() + ( originalB.x() - originalA.x() ) * t1,
                 originalA.y() + ( originalB.y() - originalA.y() ) * t1 );
    return true;
  };
  for ( int i = 1; i < vertices.size(); ++i )
  {
    QPointF a = vertices.at( i - 1 ), b = vertices.at( i );
    if ( !clipToGrid( a, b ) )
      continue;
    // supercover 整数误差项遍历：标记线段途经的全部格。穿格点（角点）时先跨
    // 竖格线再跨横格线——途经格链 (c,r)→(c+dc,r)→(c+dc,r+dr) 保持 4-连通
    // （屏障距离场不可对角穿透，45°/陡斜线不留对角漏格）；只触角点的另一侧格
    // (c,r+dr) 保持自由——贴墙探测格不因擦角被封。
    int c = colOf( a.x() ), r = rowOf( a.y() );
    const int cEnd = colOf( b.x() ), rEnd = rowOf( b.y() );
    const int dc = ( cEnd > c ) ? 1 : ( cEnd < c ? -1 : 0 );
    const int dr = ( rEnd > r ) ? 1 : ( rEnd < r ? -1 : 0 );
    const int dx = std::abs( cEnd - c ), dy = std::abs( rEnd - r );
    int err = dx - dy;
    visit( c, r );
    while ( c != cEnd || r != rEnd )
    {
      const int e2 = 2 * err;
      const bool stepX = ( c != cEnd ) && ( e2 > -dy || r == rEnd );
      const bool stepY = ( r != rEnd ) && ( e2 < dx || c == cEnd );
      if ( stepX )
      {
        c += dc;
        err -= dy;
        visit( c, r );
      }
      if ( stepY )
      {
        r += dr;
        err += dx;
        visit( c, r );
      }
      if ( !stepX && !stepY )
        break; // 防御：不推进即退出，避免异常输入死循环
    }
  }
}

int rasterizeBarrierParts( UnifiedBarrierRead &read, double originX, double originYTop, double cellSize, int cols,
                          int rows, std::vector<std::uint8_t> &mask, const std::function<bool()> &cancelled )
{
  mask.assign( static_cast<std::size_t>( rows ) * cols, 0 );
  int touchedParts = 0;
  for ( int p = 0; p < read.parts.size(); ++p )
  {
    if ( cancelled && cancelled() )
      return -1;
    const QgsPolylineXY line = read.parts.at( p ).asPolyline();
    bool touched = false;
    int newCells = 0;
    std::vector<int> marked; // 本部件落格索引（去重后即 rasterized_cells）
    const auto mark = [&]( int c, int r ) {
      marked.push_back( r * cols + c );
      if ( !mask[static_cast<std::size_t>( r ) * cols + c] )
      {
        mask[static_cast<std::size_t>( r ) * cols + c] = 1;
        ++newCells;
      }
      touched = true;
    };
    QVector<QPointF> vertices;
    vertices.reserve( line.size() );
    for ( const QgsPointXY &point : line )
      vertices.append( point.toQPointF() );
    supercoverPolylineCells( vertices, originX, originYTop, cellSize, -cellSize, cols, rows, mark );
    const int index = p < read.partReceipt.size() ? read.partReceipt.at( p ) : -1;
    if ( index < 0 || index >= read.receipts.size() )
      continue;
    QVariantMap receipt = read.receipts.at( index ).toMap();
    if ( !touched )
    {
      receipt.insert( QStringLiteral( "consumed" ), false );
      receipt.insert( QStringLiteral( "role" ), QString() );
      receipt.insert( QStringLiteral( "reason" ), QStringLiteral( "outside_compute_domain" ) );
      read.receipts[index] = receipt;
      continue;
    }
    ++touchedParts;
    // 引擎回执细化：落格部件带 rasterized/rasterized_cells/new_cells；
    // new_cells=0（完全被既有屏障遮蔽）如实标注——传给算法不等于改变数值。
    std::sort( marked.begin(), marked.end() );
    marked.erase( std::unique( marked.begin(), marked.end() ), marked.end() );
    receipt.insert( QStringLiteral( "rasterized" ), true );
    receipt.insert( QStringLiteral( "rasterized_cells" ), static_cast<int>( marked.size() ) );
    receipt.insert( QStringLiteral( "new_cells" ), newCells );
    if ( newCells == 0 )
    {
      QStringList warnings = receipt.value( QStringLiteral( "warnings" ) ).toStringList();
      warnings << QStringLiteral( "fully_shadowed_by_other_barriers" );
      receipt.insert( QStringLiteral( "warnings" ), warnings );
    }
    read.receipts[index] = receipt;
  }
  return touchedParts;
}

QString constraintReceiptsJson( const QVariantList &receipts )
{
  return QString::fromUtf8(
      QJsonDocument( QJsonArray::fromVariantList( receipts ) ).toJson( QJsonDocument::Compact ) );
}

} // namespace paleo::algorithms
