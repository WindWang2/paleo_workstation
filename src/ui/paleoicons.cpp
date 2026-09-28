// 层：视图
#include "paleoicons.h"

#include <QPainter>
#include <QPixmap>

#include <qgsapplication.h>

namespace
{
// DESIGN.md colors.text。状态色不进图标——按钮语义靠文字/tooltip 承载。
const QColor kInk( QStringLiteral( "#24303E" ) );
const QColor kPaper( QStringLiteral( "#FFFFFF" ) );

enum class Glyph { Maximize, Restore };

// 16/20/24/32 四档预渲染；描边宽度随尺寸线性缩放，避免缩放发虚。
QPixmap renderGlyph( Glyph glyph, int px )
{
  QPixmap pm( px, px );
  pm.fill( Qt::transparent );
  QPainter p( &pm );
  p.setRenderHint( QPainter::Antialiasing );
  const qreal s = px;
  const qreal w = qMax( 1.1, s * 0.085 ); // ~1.4px @16 —— 与 QGIS svg 线宽同级
  QPen pen( kInk );
  pen.setWidthF( w );
  pen.setJoinStyle( Qt::MiterJoin );
  p.setPen( pen );

  if ( glyph == Glyph::Maximize )
  {
    const qreal m = s * 0.16;
    p.setBrush( Qt::NoBrush );
    p.drawRect( QRectF( m, m, s - 2 * m, s - 2 * m ) );
  }
  else
  {
    // Restore：后框右上偏移，前框填白遮住后框左下——交叠关系一眼可读。
    p.drawRect( QRectF( s * 0.30, s * 0.10, s * 0.56, s * 0.56 ) );
    p.setBrush( kPaper );
    p.drawRect( QRectF( s * 0.12, s * 0.32, s * 0.56, s * 0.56 ) );
  }
  return pm;
}

QIcon drawnIcon( Glyph glyph )
{
  QIcon ic;
  for ( const int px : { 16, 20, 24, 32 } )
    ic.addPixmap( renderGlyph( glyph, px ) );
  return ic;
}
} // namespace

QIcon PaleoIcons::qgisTheme( const QString &name )
{
  // 与 layoutitempalette.cpp 同一惯例：前导斜杠路径交给 getThemeIcon，
  // default 主题段在 qrc（":/images/themes/default/<name>"）内解析。
  return QgsApplication::getThemeIcon( QStringLiteral( "/" ) + name );
}

QIcon PaleoIcons::maximize() { return drawnIcon( Glyph::Maximize ); }
QIcon PaleoIcons::restore() { return drawnIcon( Glyph::Restore ); }
