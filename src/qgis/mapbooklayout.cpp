// 层：QGIS 封装
#include "mapbooklayout.h"

#include "standardelements.h" // 图件字体/指北针单一事实源（方向 25 工厂）

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QFontMetricsF>
#include <QObject>
#include <QVector>

#include <algorithm>
#include <cmath>

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
  // 字体族由 standardelements 图件工厂统一（vendored "IBM Plex Sans SC" 优先
  // 链，含系统回落）——与 layoutexport 的薄别名同口径；此前这里硬编码
  // 系统族名 "Noto Sans CJK SC"，vendored 字体重叠环境下 PDF 文本无保证。
  QgsTextFormat labelFormat( const QString &family, double sizePt )
  {
    Q_UNUSED( family );
    return PaleoStandardElements::figureTextFormat( sizePt );
  }

  // 指北针 SVG：与标准图件元素工厂同一条搜索路径（单一事实源）。
  QString northArrowSvg()
  {
    return PaleoStandardElements::northArrowSvgPath();
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

  // P5-W5 目录分页参数：正文区自标题下 26mm 起、到下边距 12mm 止；8pt 条目
  // 行距 4.6mm（≥1.6 倍字高，长名换行后仍可读）。不缩字号塞页。
  constexpr double kIndexBodyTopMm = 26.0;
  constexpr double kIndexBottomMarginMm = 12.0;
  constexpr double kIndexLinePitchMm = 4.6;
  constexpr double kIndexEntrySizePt = 8.0;

  // 单条目占行数：按可用宽度与字体度量估算换行（长中文/长文件名占多行）。
  int indexEntryLines( const QFontMetricsF &metrics, const QString &entry, double availWidthPt )
  {
    if ( entry.isEmpty() )
      return 1;
    const qreal advance = metrics.horizontalAdvance( entry );
    return std::max( 1, int( std::ceil( advance / availWidthPt ) ) );
  }

  // 分页主体：全部条目按次序装进页块，每页 ≤ 按行距算出的行容量；空目录
  // 也出一页（标题页带「无可列条目」说明，调用方通常已在上游避免）。
  QVector<QStringList> paginateIndexEntries( const QStringList &entries, bool landscape )
  {
    const double ph = pageHeight( landscape );
    const double pw = pageWidth( landscape );
    const int maxLines = std::max( 1, int( ( ph - kIndexBodyTopMm - kIndexBottomMarginMm ) /
                                           kIndexLinePitchMm ) );
    const QFontMetricsF metrics( PaleoStandardElements::figureTextFormat( kIndexEntrySizePt ).font() );
    const double availWidthPt = ( pw - 20.0 ) * 72.0 / 25.4;

    QVector<QStringList> pages;
    QStringList current;
    int usedLines = 0;
    for ( const QString &entry : entries )
    {
      const int lines = indexEntryLines( metrics, entry, availWidthPt );
      if ( usedLines + lines > maxLines && !current.isEmpty() )
      {
        pages << current;
        current.clear();
        usedLines = 0;
      }
      current << entry;
      usedLines += lines;
    }
    if ( !current.isEmpty() || pages.isEmpty() )
      pages << current;
    return pages;
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
  // 地图框按格范围长宽比收进可用区域，再设范围：attemptResize 保持比例尺会把
  // 范围扩到新长宽比（相邻图幅重叠）；setExtent 只按宽度调高度，故先定好框宽。
  const double boxW = pw - 80.0;
  const double boxH = ph - 58.0;
  const double aspect = spec.extent.height() / spec.extent.width();
  double mapW = boxW;
  double mapH = boxW * aspect;
  if ( mapH > boxH )
  {
    mapH = boxH;
    mapW = boxH / aspect;
  }
  map->attemptMove( QgsLayoutPoint( 10.0, 24.0, Qgis::LayoutUnit::Millimeters ) );
  // 先按长宽比定框再 setExtent——框与范围同比例，extent 精确不重扩
  //（zoomToExtent 会把范围扩到框比 → 相邻图幅互相覆盖，不可用）。
  map->attemptResize( QgsLayoutSize( mapW, mapH, Qgis::LayoutUnit::Millimeters ) );
  map->setExtent( spec.extent );

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

  // 分页：按纸面/行高/条目占行实算（P5-W5），全部条目入册、次序不变，
  // 不靠缩字号塞页、不截断。
  const QVector<QStringList> chunks = paginateIndexEntries( spec.entries, spec.landscape );

  auto *layout = new QgsPrintLayout( project );
  setupPage( layout, QStringLiteral( "mapbook_index" ), spec.landscape );
  if ( QgsLayoutItemPage *firstPage = layout->pageCollection()->page( 0 ) )
    firstPage->setPageSize( QStringLiteral( "A4" ),
                            spec.landscape ? QgsLayoutItemPage::Landscape
                                           : QgsLayoutItemPage::Portrait );
  for ( int p = 1; p < chunks.size(); ++p )
  {
    auto *page = new QgsLayoutItemPage( layout );
    page->setPageSize( QStringLiteral( "A4" ),
                       spec.landscape ? QgsLayoutItemPage::Landscape
                                      : QgsLayoutItemPage::Portrait );
    layout->pageCollection()->addPage( page );
  }

  const int pageCount = chunks.size();
  for ( int p = 0; p < pageCount; ++p )
  {
    const QString pageTitle = pageCount > 1
        ? QStringLiteral( "%1 · 第 %2/%3 页" ).arg( spec.title ).arg( p + 1 ).arg( pageCount )
        : spec.title;
    auto *titleLabel = addLabel( layout, QStringLiteral( "title_%1" ).arg( p ), pageTitle, 14.0,
                                 10.0, 8.0, pw - 20.0, 12.0 );
    titleLabel->attemptMove( QgsLayoutPoint( 10.0, 8.0, Qgis::LayoutUnit::Millimeters ),
                             true, false, p );
    const QStringList &lines = chunks.at( p );
    const QString body = lines.isEmpty()
                             ? QObject::tr( "（本册没有可列条目）" )
                             : lines.join( QLatin1Char( '\n' ) );
    auto *bodyLabel = addLabel( layout, QStringLiteral( "entries_%1" ).arg( p ), body, 8.0,
                                10.0, 26.0, pw - 20.0, ph - 38.0 );
    bodyLabel->attemptMove( QgsLayoutPoint( 10.0, 26.0, Qgis::LayoutUnit::Millimeters ),
                            true, false, p );
  }
  return layout;
}

int indexPageCount( const QStringList &entries, bool landscape )
{
  return paginateIndexEntries( entries, landscape ).size();
}

RenderOutcome renderLayout( QgsPrintLayout *layout, const QString &outPath,
                            PaleoLayoutExport::Format format, double dpi )
{
  RenderOutcome outcome;
  const auto failExport = [&outcome]( const QString &message ) {
    outcome.ok = false;
    outcome.stage = QStringLiteral( "export" );
    outcome.error = message;
    return outcome;
  };
  if ( !layout )
    return failExport( QObject::tr( "没有可导出的版面" ) );
  if ( outPath.isEmpty() )
    return failExport( QObject::tr( "没有给出落盘路径" ) );

  const auto exported = PaleoLayoutExport::exportLayout( layout, outPath, format, dpi,
                                                          PaleoLayoutExport::PageRange() );
  if ( !exported.ok )
    return failExport( exported.error.isEmpty() ? QObject::tr( "版面导出失败" ) : exported.error );
  if ( exported.files.isEmpty() )
    return failExport( QObject::tr( "版面导出未产出文件" ) );

  const QString path = exported.files.first();
  const QFileInfo info( path );
  if ( !info.exists() || info.size() <= 0 )
    return failExport( QObject::tr( "导出产物为空或不存在：%1" ).arg( path ) );

  // PNG 期望像素 = 页面毫米规格 × dpi/25.4（多页导出的 _N 兄弟页同版
  // 同尺寸）；页面非毫米单位时跳过尺寸校验（防御）。
  double expectedW = -1.0, expectedH = -1.0;
  if ( format == PaleoLayoutExport::Format::Png && layout->pageCollection() &&
       layout->pageCollection()->pageCount() > 0 &&
       layout->pageCollection()->page( 0 ) )
  {
    const QgsLayoutSize pageSize = layout->pageCollection()->page( 0 )->sizeWithUnits();
    if ( pageSize.units() == Qgis::LayoutUnit::Millimeters )
    {
      expectedW = pageSize.width() * dpi / 25.4;
      expectedH = pageSize.height() * dpi / 25.4;
    }
  }
  // P5-W5：逐文件回读校验——首页存在不代表整册完整；每个兄弟文件都要过
  // 头校验/可解码/尺寸核对，任一失败即整次渲染失败（文件集不完整不可用）。
  qint64 totalBytes = 0;
  for ( const QString &file : exported.files )
  {
    const QFileInfo fileInfo( file );
    if ( !fileInfo.exists() || fileInfo.size() <= 0 )
      return failExport( QObject::tr( "导出产物为空或不存在：%1" ).arg( file ) );
    QString validationError;
    if ( !validateExportedFile( file, format, expectedW, expectedH, &validationError ) )
      return failExport( validationError );
    totalBytes += fileInfo.size();
  }
  if ( format == PaleoLayoutExport::Format::Png )
  {
    const QImage image( path );
    outcome.width = image.width();
    outcome.height = image.height();
  }

  outcome.ok = true;
  outcome.path = path;
  outcome.files = exported.files;
  outcome.bytes = info.size();
  outcome.totalBytes = totalBytes;
  return outcome;
}

bool validateExportedFile( const QString &path, PaleoLayoutExport::Format format,
                           double expectedWidthPx, double expectedHeightPx, QString *error )
{
  const auto fail = [error, &path]( const QString &msg ) {
    if ( error )
      *error = msg;
    return false;
  };

  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
    return fail( QObject::tr( "导出产物无法回读：%1" ).arg( path ) );
  const QByteArray head = file.read( 4096 );

  if (format == PaleoLayoutExport::Format::Tiff) {
    const QImage image(path);
    if (image.isNull()) return fail(QObject::tr("TIFF 无法解码：%1").arg(path));
    if (expectedWidthPx > 0 && expectedHeightPx > 0 &&
        (qAbs(image.width() - expectedWidthPx) > 2 || qAbs(image.height() - expectedHeightPx) > 2))
      return fail(QObject::tr("TIFF 像素尺寸与版面不符"));
    return true;
  }
  if ( format == PaleoLayoutExport::Format::Png )
  {
    if ( !head.startsWith( QByteArray( "\x89PNG\x0D\x0A\x1A\x0A", 8 ) ) )
      return fail( QObject::tr( "产物不是合法 PNG（文件头不符）：%1" ).arg( path ) );
    const QImage image( path );
    if ( image.isNull() )
      return fail( QObject::tr( "PNG 无法解码：%1" ).arg( path ) );
    // 像素尺寸 ≈ 页面毫米 × dpi/25.4（±2px 取整容差）——尺寸错说明 dpi
    // 或页面规格在导出链路上丢了，「能解码」不算成功。
    if ( expectedWidthPx > 0.0 && expectedHeightPx > 0.0 &&
         ( qAbs( image.width() - expectedWidthPx ) > 2.0 ||
           qAbs( image.height() - expectedHeightPx ) > 2.0 ) )
      return fail( QObject::tr( "PNG 像素尺寸 %1×%2 与版面期望 %3×%4 不符：%5" )
                       .arg( image.width() ).arg( image.height() )
                       .arg( QString::number( expectedWidthPx, 'f', 1 ),
                             QString::number( expectedHeightPx, 'f', 1 ), path ) );
    return true;
  }

  if ( format == PaleoLayoutExport::Format::Pdf )
  {
    if ( !head.startsWith( QByteArray( "%PDF", 4 ) ) )
      return fail( QObject::tr( "产物不是合法 PDF（文件头不符）：%1" ).arg( path ) );
    // 截断的 PDF 也能过头校验——完整 PDF 必须在末尾 1KiB 内带 %%EOF
    //（ISO 32000 trailer 标记）；缺尾即判失败，不把坏文件算进成功数。
    const qint64 tailPos = qMax<qint64>( 0, file.size() - 1024 );
    if ( !file.seek( tailPos ) )
      return fail( QObject::tr( "导出产物无法定位尾部：%1" ).arg( path ) );
    if ( !file.read( 1024 ).contains( "%%EOF" ) )
      return fail( QObject::tr( "产物 PDF 缺尾部 %%EOF（疑似截断）：%1" ).arg( path ) );
    return true;
  }

  if ( format == PaleoLayoutExport::Format::Svg )
  {
    // QGIS SVG 以 <?xml 声明开头，4KiB 头内必含 <svg 根元素。
    if ( !head.contains( "<svg" ) )
      return fail( QObject::tr( "产物不是合法 SVG（缺 <svg 根元素）：%1" ).arg( path ) );
    return true;
  }

  return fail( QObject::tr( "未知导出格式，无法校验：%1" ).arg( path ) );
}

} // namespace PaleoMapBookLayout
