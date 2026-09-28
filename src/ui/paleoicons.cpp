// 层：视图
#include "paleoicons.h"
#include "paleotheme.h"

#include <QPainter>
#include <QPixmap>
#include <QIcon>

#include <qgsapplication.h>

namespace
{
// 自绘图标的墨色随主题翻（浅色 = colors.text 深墨；暗色 = 提亮墨——
// QGIS default 主题的深 glyph 在暗底不可见，见 docs/progress/ux.md）。
// 状态色不进图标——按钮语义靠文字/tooltip 承载。
QColor inkColor() { return PaleoTheme::tokens().text; }
QColor paperColor() { return PaleoTheme::tokens().surface; }

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
  QPen pen( inkColor() );
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
    // Restore：后框右上偏移，前框填纸色遮住后框左下——交叠关系一眼可读。
    p.drawRect( QRectF( s * 0.30, s * 0.10, s * 0.56, s * 0.56 ) );
    p.setBrush( paperColor() );
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
  const QIcon base =
      QgsApplication::getThemeIcon( QStringLiteral( "/" ) + name );
  // 暗色翻案：QGIS default 主题是深 glyph（近乎单色线稿），暗底不可见。
  // 逐像素 Plus 合成提亮——深墨变浅、透明区保持透明；浅色主题原样直通。
  return PaleoTheme::currentTheme() == PaleoTheme::Theme::Dark
             ? PaleoIcons::tintForDarkTheme( base )
             : base;
}

QIcon PaleoIcons::tintForDarkTheme( const QIcon &icon )
{
  if ( icon.isNull() )
    return icon;
  // 逐像素提亮（仅 alpha>0 的像素；CompositionMode_Plus 会把透明像素
  // 也叠成不透明底——整图标套一层浅色矩形，不可用）。深墨 → 浅灰蓝，
  // 半透明边缘只提 RGB 不动 alpha。
  const QColor lift( 0xA8, 0xB4, 0xC0 );
  QIcon out;
  for ( const QSize &size : icon.availableSizes() )
  {
    const QImage src = icon.pixmap( size )
                           .toImage()
                           .convertToFormat( QImage::Format_ARGB32 );
    if ( src.isNull() )
      continue;
    QImage img( src.size(), QImage::Format_ARGB32 );
    for ( int y = 0; y < img.height(); ++y )
      for ( int x = 0; x < img.width(); ++x )
      {
        const QRgb c = src.pixel( x, y );
        if ( qAlpha( c ) == 0 )
        {
          img.setPixel( x, y, c );
          continue;
        }
        img.setPixel( x, y,
                      qRgba( qMin( 255, qRed( c ) + lift.red() ),
                             qMin( 255, qGreen( c ) + lift.green() ),
                             qMin( 255, qBlue( c ) + lift.blue() ),
                             qAlpha( c ) ) );
      }
    out.addPixmap( QPixmap::fromImage( img ) );
  }
  return out;
}

QIcon PaleoIcons::maximize() { return drawnIcon( Glyph::Maximize ); }
QIcon PaleoIcons::restore() { return drawnIcon( Glyph::Restore ); }
