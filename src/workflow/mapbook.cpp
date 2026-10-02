// 层：功能
#include "mapbook.h"

#include <QDate>
#include <QLatin1String>

namespace PaleoMapBook
{

namespace
{
  // 两位小数、不带多余尾零的坐标串（版面文本统一口径）。
  QString num( double v )
  {
    QString s = QString::number( v, 'f', 2 );
    while ( s.contains( QLatin1Char( '.' ) ) && s.endsWith( QLatin1Char( '0' ) ) )
      s.chop( 1 );
    if ( s.endsWith( QLatin1Char( '.' ) ) )
      s.chop( 1 );
    return s;
  }

  // `%{name}` → 替换 / 报错。返回 false 表示 *error 已写。
  bool substitute( const QString &text, const QVariantMap &vars, QString *out, QString *error )
  {
    const QLatin1String open( "%{" );
    QString result;
    int i = 0;
    while ( i < text.size() )
    {
      const int at = text.indexOf( open, i );
      if ( at < 0 )
      {
        result += text.mid( i );
        break;
      }
      result += text.mid( i, at - i );
      const int close = text.indexOf( QLatin1Char( '}' ), at + 2 );
      if ( close < 0 )
      {
        if ( error )
          *error = QObject::tr( "模板变量未闭合（缺 '}'）：%1" ).arg( text.mid( at ) );
        return false;
      }
      const QString name = text.mid( at + 2, close - at - 2 ).trimmed();
      if ( !vars.contains( name ) )
      {
        if ( error )
          *error = QObject::tr( "模板变量 %1 未定义（可用变量：%2）" )
                     .arg( name, vars.keys().join( QStringLiteral( ", " ) ) );
        return false;
      }
      result += vars.value( name ).toString();
      i = close + 1;
    }
    *out = result;
    return true;
  }
} // namespace

QString Area::extentText() const
{
    return QStringLiteral( "%1, %2 → %3, %4" ).arg( num( xMin ), num( yMin ), num( xMax ), num( yMax ) );
}

QVector<Tile> buildGrid( const GridRequest &request, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return QVector<Tile>();
  };
  if ( !request.area.valid() )
    return fail( QObject::tr( "AOI 范围不合法（需 xMax>xMin 且 yMax>yMin）" ) );
  if ( request.cols < 1 || request.rows < 1 )
    return fail( QObject::tr( "网格行列数必须 ≥1（cols=%1, rows=%2）" )
                   .arg( request.cols ).arg( request.rows ) );

  const double cellW = request.area.width() / request.cols;
  const double cellH = request.area.height() / request.rows;

  QVector<Tile> tiles;
  tiles.reserve( request.cols * request.rows );
  // 生成次序 = 遍历次序（列主序时外层走列），index 因此天然连续。
  const int outer = ( request.order == Order::RowMajor ) ? request.rows : request.cols;
  const int inner = ( request.order == Order::RowMajor ) ? request.cols : request.rows;
  for ( int a = 0; a < outer; ++a )
  {
    for ( int b = 0; b < inner; ++b )
    {
      const int r = ( request.order == Order::RowMajor ) ? a : b;
      const int c = ( request.order == Order::RowMajor ) ? b : a;
      // 格边界按序号线性插值：末格直接取 AOI 上界，浮点误差不累积到边。
      const double x0 = request.area.xMin + c * cellW;
      const double x1 = ( c == request.cols - 1 ) ? request.area.xMax : request.area.xMin + ( c + 1 ) * cellW;
      const double y0 = request.area.yMin + r * cellH;
      const double y1 = ( r == request.rows - 1 ) ? request.area.yMax : request.area.yMin + ( r + 1 ) * cellH;

      Tile tile;
      tile.row = r + 1;
      tile.col = c + 1;
      tile.index = tiles.size();
      tile.extent = Area{ x0, y0, x1, y1, QString() };

      QString nameErr;
      QString name;
      const QVariantMap vars =
          tileVariables( TileContext{ tile, QString(), QString(), QString(), QString() } );
      if ( !substitute( request.namePattern, vars, &name, &nameErr ) )
        return fail( nameErr );
      tile.name = name;
      tiles.append( tile );
    }
  }
  return tiles;
}

QVector<Tile> tilesFromAreas( const QVector<Area> &areas, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return QVector<Tile>();
  };
  if ( areas.isEmpty() )
    return fail( QObject::tr( "没有给定任何分区范围" ) );

  QVector<Tile> tiles;
  tiles.reserve( areas.size() );
  for ( int i = 0; i < areas.size(); ++i )
  {
    if ( !areas.at( i ).valid() )
      return fail( QObject::tr( "第 %1 个分区范围不合法（%2）" ).arg( i + 1 ).arg( areas.at( i ).name ) );
    Tile tile;
    tile.index = i;
    tile.row = 1;
    tile.col = i + 1;
    tile.extent = areas.at( i );
    const QString pattern = areas.at( i ).name.isEmpty() ? QStringLiteral( "area_%{tile_index}" )
                                                         : areas.at( i ).name;
    QString nameErr;
    QString name;
    QVariantMap vars = tileVariables( TileContext{ tile, QString(), QString(), QString(), QString() } );
    if ( !substitute( pattern, vars, &name, &nameErr ) )
      return fail( nameErr );
    tile.name = name;
    tiles.append( tile );
  }
  return tiles;
}

QStringList variableNames()
{
  return QStringList( { QStringLiteral( "tile_index" ), QStringLiteral( "tile_row" ),
                        QStringLiteral( "tile_col" ), QStringLiteral( "tile_name" ),
                        QStringLiteral( "tile_label" ), QStringLiteral( "center_x" ),
                        QStringLiteral( "center_y" ), QStringLiteral( "x_min" ),
                        QStringLiteral( "y_min" ), QStringLiteral( "x_max" ),
                        QStringLiteral( "y_max" ), QStringLiteral( "width" ),
                        QStringLiteral( "height" ), QStringLiteral( "extent" ),
                        QStringLiteral( "date" ), QStringLiteral( "book" ),
                        QStringLiteral( "horizon" ), QStringLiteral( "crs" ) } );
}

QVariantMap tileVariables( const TileContext &context )
{
  const Tile &t = context.tile;
  QVariantMap vars;
  vars.insert( QStringLiteral( "tile_index" ), QString::number( t.index ) );
  vars.insert( QStringLiteral( "tile_row" ), QString::number( t.row ) );
  vars.insert( QStringLiteral( "tile_col" ), QString::number( t.col ) );
  vars.insert( QStringLiteral( "tile_name" ), t.name );
  // tile_label：`R1C1` 式短标（版面角标/索引页用，比全名省地方）。
  vars.insert( QStringLiteral( "tile_label" ),
               QStringLiteral( "R%1C%2" ).arg( t.row ).arg( t.col ) );
  vars.insert( QStringLiteral( "center_x" ), num( t.extent.centerX() ) );
  vars.insert( QStringLiteral( "center_y" ), num( t.extent.centerY() ) );
  vars.insert( QStringLiteral( "x_min" ), num( t.extent.xMin ) );
  vars.insert( QStringLiteral( "y_min" ), num( t.extent.yMin ) );
  vars.insert( QStringLiteral( "x_max" ), num( t.extent.xMax ) );
  vars.insert( QStringLiteral( "y_max" ), num( t.extent.yMax ) );
  vars.insert( QStringLiteral( "width" ), num( t.extent.width() ) );
  vars.insert( QStringLiteral( "height" ), num( t.extent.height() ) );
  vars.insert( QStringLiteral( "extent" ), t.extent.extentText() );
  vars.insert( QStringLiteral( "date" ), context.date.isEmpty()
                                           ? QDate::currentDate().toString( Qt::ISODate )
                                           : context.date );
  vars.insert( QStringLiteral( "book" ), context.book );
  vars.insert( QStringLiteral( "horizon" ), context.horizon );
  vars.insert( QStringLiteral( "crs" ), context.crs.isEmpty()
                                          ? QObject::tr( "工程坐标 · 米 · 未投影" )
                                          : context.crs );
  return vars;
}

QString applyVariables( const QString &text, const QVariantMap &vars, QString *error )
{
  QString out;
  if ( !substitute( text, vars, &out, error ) )
    return QString();
  return out;
}

QStringList referencedVariables( const QString &text, QString *error )
{
  const QLatin1String open( "%{" );
  QStringList names;
  int i = 0;
  while ( i < text.size() )
  {
    const int at = text.indexOf( open, i );
    if ( at < 0 )
      break;
    const int close = text.indexOf( QLatin1Char( '}' ), at + 2 );
    if ( close < 0 )
    {
      if ( error )
        *error = QObject::tr( "模板变量未闭合（缺 '}'）：%1" ).arg( text.mid( at ) );
      return QStringList();
    }
    const QString name = text.mid( at + 2, close - at - 2 ).trimmed();
    if ( !names.contains( name ) )
      names << name;
    i = close + 1;
  }
  return names;
}

} // namespace PaleoMapBook
