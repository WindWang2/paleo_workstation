// 层：QGIS 封装
#include "mapbooklayout.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QObject>

#include <qgsapplication.h>
#include <qgslayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemlegend.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutpoint.h>
#include <qgslayoutsize.h>
#include <qgsmaplayer.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsrectangle.h>
#include <qgstextformat.h>

namespace PaleoMapBookLayout
{

namespace
{
  // A4 版面（mm）。横版 297×210，竖版 210×297。
  double pageWidth( bool landscape ) { return landscape ? 297.0 : 210.0; }
  double pageHeight( bool landscape ) { return landscape ? 210.0 : 297.0; }

  // 标签字体（QGIS 4.x：setFont 已弃用，走 QgsTextFormat；字号单位 pt）。
  QgsTextFormat labelFormat( const QString &family, double sizePt )
  {
    QgsTextFormat fmt;
    QFont font( family );
    font.setPointSizeF( sizePt );
    fmt.setFont( font );
    fmt.setSizeUnit( Qgis::RenderUnit::Points );
    fmt.setSize( sizePt );
    return fmt;
  }

  // 指北针 SVG：QGIS 自带 arrows/NorthArrow_*.svg。
  QString northArrowSvg()
  {
    for ( const QString &root : QgsApplication::svgPaths() )
      for ( const QString &name :
            { QStringLiteral( "NorthArrow_04.svg" ), QStringLiteral( "NorthArrow_02.svg" ),
              QStringLiteral( "NorthArrow_01.svg" ), QStringLiteral( "NorthArrow_03.svg" ),
              QStringLiteral( "NorthArrow_05.svg" ), QStringLiteral( "NorthArrow_06.svg" ) } )
      {
        const QString path = QDir( root ).filePath( QStringLiteral( "arrows/" ) + name );
        if ( QFile::exists( path ) )
          return path;
      }
    return QString();
  }

  QgsLayoutItemLabel *addLabel( QgsPrintLayout *layout, const QString &id, const QString &text,
                                double sizePt, double x, double y, double w, double h )
  {
    auto *label = new QgsLayoutItemLabel( layout );
    label->setId( id );
    label->setText( text );
    label->setTextFormat( labelFormat( QStringLiteral( "Noto Sans CJK SC" ), sizePt ) );
    layout->addLayoutItem( label );
    label->attemptResize( QgsLayoutSize( w, h, Qgis::LayoutUnit::Millimeters ) );
    label->attemptMove( QgsLayoutPoint( x, y, Qgis::LayoutUnit::Millimeters ) );
    return label;
  }

  // 页脚/小标题的统一落位（左下或图项下方）。
  QgsLayoutItemPicture *addPicture( QgsPrintLayout *layout, const QString &id,
                                    const QString &path, double x, double y, double w, double h )
  {
    auto *picture = new QgsLayoutItemPicture( layout );
    picture->setId( id );
    picture->setPicturePath( path, Qgis::PictureFormat::Raster );
    picture->setResizeMode( QgsLayoutItemPicture::Zoom );
    layout->addLayoutItem( picture );
    picture->attemptResize( QgsLayoutSize( w, h, Qgis::LayoutUnit::Millimeters ) );
    picture->attemptMove( QgsLayoutPoint( x, y, Qgis::LayoutUnit::Millimeters ) );
    return picture;
  }

  void setupPage( QgsPrintLayout *layout, const QString &name, bool landscape )
  {
    layout->initializeDefaults();
    layout->setName( name );
    if ( QgsLayoutItemPage *page = layout->pageCollection()->page( 0 ) )
      page->setPageSize( QStringLiteral( "A4" ),
                         landscape ? QgsLayoutItemPage::Landscape : QgsLayoutItemPage::Portrait );
  }
} // namespace

QgsPrintLayout *buildTileLayout( QgsProject *project, const TileSpec &spec, QString *error )
{
  const auto fail = [error]( const QString &msg ) -> QgsPrintLayout * {
    if ( error )
      *error = msg;
    return nullptr;
  };
  if ( !project )
    return fail( QObject::tr( "未绑定工程（版面需要 QgsProject）" ) );
  if ( spec.extent.isEmpty() || spec.extent.width() <= 0.0 || spec.extent.height() <= 0.0 )
    return fail( QObject::tr( "格范围为空或退化（宽 %1 高 %2）" )
                   .arg( spec.extent.width() ).arg( spec.extent.height() ) );

  const double pw = pageWidth( spec.landscape );
  const double ph = pageHeight( spec.landscape );

  auto *layout = new QgsPrintLayout( project );
  setupPage( layout, QStringLiteral( "mapbook_tile" ), spec.landscape );

  auto *map = new QgsLayoutItemMap( layout );
  map->setId( QStringLiteral( "map" ) );
  map->setLayers( spec.layers );
  layout->addLayoutItem( map );
  map->attemptResize( QgsLayoutSize( pw - 80.0, ph - 58.0, Qgis::LayoutUnit::Millimeters ) );
  map->attemptMove( QgsLayoutPoint( 10.0, 24.0, Qgis::LayoutUnit::Millimeters ) );
  // A map starts at zero size. Set the range after sizing to avoid NaN and
  // expand to the frame's aspect ratio so the entire tile remains visible.
  map->zoomToExtent( spec.extent );

  addLabel( layout, QStringLiteral( "title" ), spec.title, 14.0, 10.0, 8.0, pw - 20.0, 12.0 );

  if ( spec.legend )
  {
    auto *legend = new QgsLayoutItemLegend( layout );
    legend->setId( QStringLiteral( "legend" ) );
    legend->setTitle( QObject::tr( "图例" ) );
    legend->setLinkedMap( map );
    legend->setSyncMode( Qgis::LegendSyncMode::VisibleLayers );
    legend->setLegendFilterByMapEnabled( true );
    layout->addLayoutItem( legend );
    legend->attemptResize( QgsLayoutSize( 58.0, 60.0, Qgis::LayoutUnit::Millimeters ) );
    legend->attemptMove( QgsLayoutPoint( pw - 68.0, 26.0, Qgis::LayoutUnit::Millimeters ) );
  }

  if ( spec.scaleBar )
  {
    auto *scalebar = new QgsLayoutItemScaleBar( layout );
    scalebar->setId( QStringLiteral( "scalebar" ) );
    scalebar->setLinkedMap( map );
    scalebar->setUnits( Qgis::DistanceUnit::Meters );
    scalebar->setUnitLabel( QStringLiteral( "m" ) );
    scalebar->applyDefaultSettings();
    scalebar->applyDefaultSize( Qgis::DistanceUnit::Meters );
    layout->addLayoutItem( scalebar );
    scalebar->attemptMove( QgsLayoutPoint( 12.0, ph - 28.0, Qgis::LayoutUnit::Millimeters ) );
    scalebar->update();
  }

  if ( spec.northArrow )
  {
    const QString arrowSvg = northArrowSvg();
    if ( !arrowSvg.isEmpty() )
    {
      auto *arrow = new QgsLayoutItemPicture( layout );
      arrow->setId( QStringLiteral( "northArrow" ) );
      arrow->setMode( Qgis::PictureFormat::SVG );
      arrow->setPicturePath( arrowSvg );
      arrow->setLinkedMap( map ); // 随地图旋转
      layout->addLayoutItem( arrow );
      arrow->attemptResize( QgsLayoutSize( 14.0, 14.0, Qgis::LayoutUnit::Millimeters ) );
      arrow->attemptMove( QgsLayoutPoint( pw - 26.0, 10.0, Qgis::LayoutUnit::Millimeters ) );
    }
    else
    {
      addLabel( layout, QStringLiteral( "northArrow" ), QStringLiteral( "N" ), 12.0,
                pw - 26.0, 10.0, 10.0, 10.0 );
    }
  }

  if ( spec.crsCaption || !spec.footer.isEmpty() )
    addLabel( layout, QStringLiteral( "footer" ), spec.footer, 9.0,
              10.0, ph - 14.0, pw - 20.0, 8.0 );

  return layout;
}

QgsPrintLayout *buildMontageLayout( QgsProject *project, const MontageSpec &spec, QString *error )
{
  const auto fail = [error]( const QString &msg ) -> QgsPrintLayout * {
    if ( error )
      *error = msg;
    return nullptr;
  };
  if ( !project )
    return fail( QObject::tr( "未绑定工程（版面需要 QgsProject）" ) );
  if ( spec.mapExtent.isEmpty() || spec.mapExtent.width() <= 0.0 || spec.mapExtent.height() <= 0.0 )
    return fail( QObject::tr( "平面图范围为空或退化" ) );
  if ( spec.sectionImage.isEmpty() && spec.wellImage.isEmpty() )
    return fail( QObject::tr( "蒙太奇至少要有剖面快照或连井小图之一" ) );

  const double pw = pageWidth( spec.landscape );
  const double ph = pageHeight( spec.landscape );

  // 两栏几何一律由页面尺寸推导：右栏 x 若写死成 pw-147，竖版 A4 下会压到左栏平面图上。
  const double margin = 10.0;
  const double bodyY = 24.0;
  const double colGap = 8.0;
  const double captionH = 8.0;
  const double rowGap = 6.0;
  const double bodyH = ph - bodyY - margin;
  const double usableW = pw - 2.0 * margin - colGap;
  const double leftW = usableW * 0.56;
  const double rightW = usableW - leftW;
  const double rightX = margin + leftW + colGap;

  auto *layout = new QgsPrintLayout( project );
  setupPage( layout, QStringLiteral( "mapbook_montage" ), spec.landscape );
  addLabel( layout, QStringLiteral( "title" ), spec.title, 14.0,
            margin, 8.0, pw - 2.0 * margin, 12.0 );

  // 左：平面图（真地图项，与剖面/连井同一 AOI 语义）。
  const double planH = bodyH - captionH - 2.0;
  auto *map = new QgsLayoutItemMap( layout );
  map->setId( QStringLiteral( "planMap" ) );
  map->setLayers( spec.mapLayers );
  layout->addLayoutItem( map );
  map->attemptResize( QgsLayoutSize( leftW, planH, Qgis::LayoutUnit::Millimeters ) );
  map->zoomToExtent( spec.mapExtent );
  map->attemptMove( QgsLayoutPoint( margin, bodyY, Qgis::LayoutUnit::Millimeters ) );
  addLabel( layout, QStringLiteral( "planMapCaption" ), spec.mapCaption, 9.0,
            margin, bodyY + planH + 2.0, leftW, captionH );

  // 右：剖面快照 / 连井小图上下均分（渲染管线出的 PNG，经图片项进版面）。
  const int cells = ( spec.sectionImage.isEmpty() ? 0 : 1 ) + ( spec.wellImage.isEmpty() ? 0 : 1 );
  const double cellH = ( bodyH - ( cells > 1 ? rowGap : 0.0 ) ) / static_cast<double>( cells );
  const double picH = cellH - captionH - 2.0;
  int cell = 0;
  if ( !spec.sectionImage.isEmpty() )
  {
    const double y = bodyY + static_cast<double>( cell ) * ( cellH + rowGap );
    addPicture( layout, QStringLiteral( "sectionSnapshot" ), spec.sectionImage,
                rightX, y, rightW, picH );
    addLabel( layout, QStringLiteral( "sectionCaption" ), spec.sectionCaption, 9.0,
              rightX, y + picH + 2.0, rightW, captionH );
    ++cell;
  }
  if ( !spec.wellImage.isEmpty() )
  {
    const double y = bodyY + static_cast<double>( cell ) * ( cellH + rowGap );
    addPicture( layout, QStringLiteral( "wellPanel" ), spec.wellImage,
                rightX, y, rightW, picH );
    addLabel( layout, QStringLiteral( "wellCaption" ), spec.wellCaption, 9.0,
              rightX, y + picH + 2.0, rightW, captionH );
    ++cell;
  }

  return layout;
}

QgsPrintLayout *buildIndexLayout( QgsProject *project, const IndexSpec &spec, QString *error )
{
  const auto fail = [error]( const QString &msg ) -> QgsPrintLayout * {
    if ( error )
      *error = msg;
    return nullptr;
  };
  if ( !project )
    return fail( QObject::tr( "未绑定工程（版面需要 QgsProject）" ) );

  const double pw = pageWidth( spec.landscape );
  const double ph = pageHeight( spec.landscape );

  auto *layout = new QgsPrintLayout( project );
  setupPage( layout, QStringLiteral( "mapbook_index" ), spec.landscape );
  addLabel( layout, QStringLiteral( "title" ), spec.title, 14.0, 10.0, 8.0, pw - 20.0, 12.0 );

  // 目录正文：一行一条，条目过多时只印前 60 行并附省略说明（版面不溢出）。
  QStringList lines = spec.entries;
  const int maxLines = 60;
  if ( lines.size() > maxLines )
    lines = lines.mid( 0, maxLines ) + QStringList(
              QObject::tr( "… 其余 %1 条见 manifest.json" ).arg( spec.entries.size() - maxLines ) );

  addLabel( layout, QStringLiteral( "entries" ), lines.join( QLatin1Char( '\n' ) ), 8.0,
            10.0, 26.0, pw - 20.0, ph - 40.0 );
  return layout;
}

RenderOutcome renderLayout( QgsPrintLayout *layout, const QString &outPath,
                            PaleoLayoutExport::Format format, double dpi )
{
  RenderOutcome outcome;
  if ( !layout )
  {
    outcome.error = QObject::tr( "没有可导出的版面" );
    return outcome;
  }
  if ( outPath.isEmpty() )
  {
    outcome.error = QObject::tr( "没有给出落盘路径" );
    return outcome;
  }

  const auto exported = PaleoLayoutExport::exportLayout( layout, outPath, format, dpi,
                                                          PaleoLayoutExport::PageRange() );
  if ( !exported.ok )
  {
    outcome.error = exported.error.isEmpty() ? QObject::tr( "版面导出失败" ) : exported.error;
    return outcome;
  }
  if ( exported.files.isEmpty() )
  {
    outcome.error = QObject::tr( "版面导出未产出文件" );
    return outcome;
  }

  const QString path = exported.files.first();
  const QFileInfo info( path );
  if ( !info.exists() || info.size() <= 0 )
  {
    outcome.error = QObject::tr( "导出产物为空或不存在：%1" ).arg( path );
    return outcome;
  }

  // 回读校验（Oracle「文件头格式校验」）：PNG 验 8 字节签名 + 像素尺寸，
  // PDF 验 %PDF 头；读不出来的产物直接判失败，不把坏文件算进成功数。
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
  {
    outcome.error = QObject::tr( "导出产物无法回读：%1" ).arg( path );
    return outcome;
  }
  const QByteArray head = file.read( 8 );
  file.close();

  if ( format == PaleoLayoutExport::Format::Png )
  {
    if ( head != QByteArray( "\x89PNG\x0D\x0A\x1A\x0A", 8 ) )
    {
      outcome.error = QObject::tr( "产物不是合法 PNG（文件头不符）：%1" ).arg( path );
      return outcome;
    }
    const QImage image( path );
    if ( image.isNull() )
    {
      outcome.error = QObject::tr( "PNG 无法解码：%1" ).arg( path );
      return outcome;
    }
    outcome.width = image.width();
    outcome.height = image.height();
  }
  else if ( format == PaleoLayoutExport::Format::Pdf )
  {
    if ( !head.startsWith( QByteArray( "%PDF", 4 ) ) )
    {
      outcome.error = QObject::tr( "产物不是合法 PDF（文件头不符）：%1" ).arg( path );
      return outcome;
    }
  }

  outcome.ok = true;
  outcome.path = path;
  outcome.bytes = info.size();
  return outcome;
}

} // namespace PaleoMapBookLayout
