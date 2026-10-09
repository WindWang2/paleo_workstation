// 层：QGIS 封装
#include "layoutexport.h"
#include "standardelements.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QObject>
#include <QSet>

#include <memory>

#include <gdal.h>

#include <qgsapplication.h>
#include <qgslayout.h>
#include <qgslayoutexporter.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemlegend.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutpagecollection.h>
#include <qgsmaplayer.h>
#include <qgspallabeling.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>
#include <qgsrastertransparency.h>
#include <qgsrectangle.h>
#include <qgscoordinatetransform.h>
#include <qgsprintlayout.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>
#include <qgstextformat.h>

#include "qgislayerservice.h"
#include "qgisprojectservice.h"
#include "../metadata/layermanifest.h"

namespace
{
  // Effective destination path: append the format's default extension when the
  // caller-provided path has none (QImage/QPdfWriter derive the writer from it).
  QString withDefaultExtension( const QString &path, PaleoLayoutExport::Format format )
  {
    const char *ext = nullptr;
    switch ( format )
    {
      case PaleoLayoutExport::Format::Png: ext = "png"; break;
      case PaleoLayoutExport::Format::Pdf: ext = "pdf"; break;
      case PaleoLayoutExport::Format::Svg: ext = "svg"; break;
    }
    const QFileInfo info( path );
    if ( info.suffix().compare( QLatin1String( ext ), Qt::CaseInsensitive ) == 0 )
      return path;
    return path + QLatin1Char( '.' ) + QLatin1String( ext );
  }

  QString resultErrorText( QgsLayoutExporter::ExportResult result, const QgsLayoutExporter &exporter )
  {
    QString error;
    switch ( result )
    {
      case QgsLayoutExporter::Success: return QString();
      case QgsLayoutExporter::Canceled: error = QObject::tr( "导出已取消。" ); break;
      case QgsLayoutExporter::MemoryError: error = QObject::tr( "内存不足，无法导出图件。" ); break;
      case QgsLayoutExporter::FileError:
        error = QObject::tr( "无法写入导出文件 %1。" ).arg( exporter.errorFile() );
        break;
      case QgsLayoutExporter::PrintError: error = QObject::tr( "无法开始打印导出。" ); break;
      case QgsLayoutExporter::SvgLayerError: error = QObject::tr( "无法创建分层 SVG 文件。" ); break;
      case QgsLayoutExporter::IteratorError: error = QObject::tr( "遍历图件页面时出错。" ); break;
    }
    if ( !exporter.errorMessage().isEmpty() )
      error += QLatin1Char( ' ' ) + exporter.errorMessage();
    return error;
  }

  // QGIS 4.2 Pdf/Svg export settings have no page list: a page range is
  // honored by exporting a trimmed clone. The source layout is untouched.
  // deletePage()'s reflow relocates the kept pages' items along with the
  // pages (probed: an item on page 2 lands at the top of the renumbered page
  // 1), so only the deleted pages' own content items are removed explicitly.
  std::unique_ptr<QgsLayout> trimmedClone( QgsLayout *layout, const QList<int> &keepPages )
  {
    std::unique_ptr<QgsLayout> clone( layout->clone() );
    if ( !clone )
      return nullptr;

    QSet<int> keep( keepPages.cbegin(), keepPages.cend() );
    const int total = clone->pageCollection()->pageCount();
    for ( int page = total - 1; page >= 0; --page )
    {
      if ( keep.contains( page ) )
        continue;
      // Delete the page's content items (but never page items themselves).
      const QList<QgsLayoutItem *> doomed = clone->pageCollection()->itemsOnPage( page );
      for ( QgsLayoutItem *item : doomed )
      {
        if ( !dynamic_cast<QgsLayoutItemPage *>( item ) )
          clone->removeLayoutItem( item );
      }
      clone->pageCollection()->deletePage( page );
    }
    return clone;
  }
}

namespace PaleoLayoutExport
{

ExportOutcome exportLayout( QgsLayout *layout, const QString &outPath, Format format,
                            double dpi, const PageRange &range )
{
  ExportOutcome outcome;

  if ( !layout )
  {
    outcome.error = QObject::tr( "没有可导出的图件。" );
    return outcome;
  }
  if ( outPath.isEmpty() )
  {
    outcome.error = QObject::tr( "未指定导出文件。" );
    return outcome;
  }

  const QString path = withDefaultExtension( outPath, format );
  outcome.effectivePath = path;
  const int pageCount = layout->pageCollection() ? layout->pageCollection()->pageCount() : 0;
  if ( pageCount < 1 )
  {
    outcome.error = QObject::tr( "该图件没有可导出的页面。" );
    return outcome;
  }

  // Resolve the page selection (0-based).
  QList<int> pages;
  switch ( range.mode )
  {
    case PageRange::Mode::All:
      for ( int i = 0; i < pageCount; ++i )
        pages << i;
      break;
    case PageRange::Mode::Current:
      pages << qBound( 0, range.currentPage, pageCount - 1 );
      break;
    case PageRange::Mode::Range:
    {
      const int from = qMax( 0, range.fromPage );
      const int to = qMin( pageCount - 1, range.toPage );
      if ( from > to )
      {
        outcome.error = QObject::tr( "页码范围没有落在这套 %1 页的图件上。" ).arg( pageCount );
        return outcome;
      }
      for ( int i = from; i <= to; ++i )
        pages << i;
      break;
    }
  }

  QgsLayoutExporter::ExportResult result = QgsLayoutExporter::Success;
  QString extraError;

  // The native pages list renumbers output files by ORIGINAL page number
  // (exporting page 1 yields "base_2.png", not "base.png" — probed), so it is
  // only used while the selection is a contiguous run starting at the first
  // page; every other selection is renumbered through the trimmed clone so
  // outPath always receives the first selected page.
  const bool contiguousFromZero = !pages.isEmpty() && pages.first() == 0 && pages.last() == pages.size() - 1;

  switch ( format )
  {
    case Format::Png:
    {
      if ( contiguousFromZero )
      {
        QgsLayoutExporter exporter( layout );
        QgsLayoutExporter::ImageExportSettings settings;
        settings.dpi = dpi;
        settings.pages = pages; // empty would mean "all"; pages always covers All here
        result = exporter.exportToImage( path, settings );
        extraError = resultErrorText( result, exporter );
      }
      else
      {
        std::unique_ptr<QgsLayout> clone = trimmedClone( layout, pages );
        if ( !clone )
        {
          outcome.error = QObject::tr( "无法准备要导出的页面。" );
          return outcome;
        }
        QgsLayoutExporter exporter( clone.get() );
        QgsLayoutExporter::ImageExportSettings settings;
        settings.dpi = dpi;
        result = exporter.exportToImage( path, settings );
        extraError = resultErrorText( result, exporter );
      }
      break;
    }
    case Format::Pdf:
    {
      if ( pages.size() == pageCount )
      {
        QgsLayoutExporter exporter( layout );
        QgsLayoutExporter::PdfExportSettings settings;
        settings.dpi = dpi;
        result = exporter.exportToPdf( path, settings );
        extraError = resultErrorText( result, exporter );
      }
      else
      {
        std::unique_ptr<QgsLayout> clone = trimmedClone( layout, pages );
        if ( !clone )
        {
          outcome.error = QObject::tr( "无法准备要导出的页面。" );
          return outcome;
        }
        QgsLayoutExporter exporter( clone.get() );
        QgsLayoutExporter::PdfExportSettings settings;
        settings.dpi = dpi;
        result = exporter.exportToPdf( path, settings );
        extraError = resultErrorText( result, exporter );
      }
      break;
    }
    case Format::Svg:
    {
      if ( pages.size() == pageCount )
      {
        QgsLayoutExporter exporter( layout );
        QgsLayoutExporter::SvgExportSettings settings;
        settings.dpi = dpi;
        result = exporter.exportToSvg( path, settings );
        extraError = resultErrorText( result, exporter );
      }
      else
      {
        std::unique_ptr<QgsLayout> clone = trimmedClone( layout, pages );
        if ( !clone )
        {
          outcome.error = QObject::tr( "无法准备要导出的页面。" );
          return outcome;
        }
        QgsLayoutExporter exporter( clone.get() );
        QgsLayoutExporter::SvgExportSettings settings;
        settings.dpi = dpi;
        result = exporter.exportToSvg( path, settings );
        extraError = resultErrorText( result, exporter );
      }
      break;
    }
  }

  if ( result == QgsLayoutExporter::Success )
  {
    outcome.ok = true;
    outcome.files << path;

    // Multi-file formats (PNG/SVG over multiple pages) make QGIS write the
    // primary file plus "<base>_2.<ext>", "<base>_3.<ext>"... siblings.
    const QFileInfo info( path );
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    QStringList siblings;
    for ( const QString &name : QDir( info.absolutePath() ).entryList( QDir::Files ) )
    {
      const QFileInfo siblingInfo( name );
      if ( !siblingInfo.completeBaseName().startsWith( base + QLatin1Char( '_' ) ) )
        continue;
      if ( siblingInfo.suffix().compare( suffix, Qt::CaseInsensitive ) != 0 )
        continue;
      // Only "_N" siblings for a page that could have been exported
      // (selections are renumbered, so N runs 2..selected page count).
      const QString pageToken = siblingInfo.completeBaseName().mid( base.length() + 1 );
      bool numeric = false;
      const int pageNumber = pageToken.toInt( &numeric );
      if ( !numeric || pageNumber < 2 || pageNumber > pages.size() )
        continue;
      const QString abs = info.absolutePath() + QLatin1Char( '/' ) + name;
      if ( abs != path )
        siblings << abs;
    }
    siblings.sort();
    outcome.files << siblings;
  }
  else
  {
    outcome.error = extraError.isEmpty() ? QObject::tr( "导出失败。" ) : extraError;
  }

  return outcome;
}

namespace
{
  // Survey extent from a raster source (geotransform); empty rectangle when
  // the raster cannot be read.
  QgsRectangle rasterExtent( const QString &source )
  {
    const QString path = source.section( QLatin1Char( '|' ), 0, 0 );
    if ( path.isEmpty() || !QFile::exists( path ) )
      return QgsRectangle();
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
    if ( !ds )
      return QgsRectangle();
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    GDALGetGeoTransform( ds, gt );
    const int cols = GDALGetRasterXSize( ds );
    const int rows = GDALGetRasterYSize( ds );
    GDALClose( ds );
    if ( cols <= 0 || rows <= 0 || gt[1] <= 0.0 )
      return QgsRectangle();
    return QgsRectangle( gt[0], gt[3] + gt[5] * rows, gt[0] + gt[1] * cols, gt[3] );
  }

  // nodata -9999 不画（§162）：单值透明表在渲染器上。
  void hideNodata( QgsMapLayer *layer )
  {
    auto *raster = qobject_cast<QgsRasterLayer *>( layer );
    if ( !raster || !raster->renderer() )
      return;
    auto *transparency = new QgsRasterTransparency; // renderer 接管所有权
    QgsRasterTransparency::TransparentSingleValuePixel px;
    px.min = -9999.0;
    px.max = -9999.0;
    px.opacity = 0.0; // 0 = 全透明
    transparency->setTransparentSingleValuePixelList( { px } );
    raster->renderer()->setRasterTransparency( transparency );
  }

  // 井名标注：分层点图层的 well_name 字段（§162「井名」）。
  void enableWellNameLabels( QgsMapLayer *layer )
  {
    auto *vl = qobject_cast<QgsVectorLayer *>( layer );
    if ( !vl || vl->fields().indexOf( QStringLiteral( "well_name" ) ) < 0 )
      return;
    QgsPalLayerSettings settings;
    settings.fieldName = QStringLiteral( "well_name" );
    settings.isExpression = false;
    vl->setLabeling( new QgsVectorLayerSimpleLabeling( settings ) );
    vl->setLabelsEnabled( true );
  }

  // 标签字体/指北针 SVG：与标准图件元素工厂共用同一份（standardelements，
  // 方向 25 起为单一事实源——这里只留薄别名，调用点写法不变）。
  QgsTextFormat labelFormat( const QString &family, double sizePt )
  {
    Q_UNUSED( family ); // 字体族由工厂统一（Noto Sans CJK SC，DESIGN.md）
    return PaleoStandardElements::figureTextFormat( sizePt );
  }

  QString northArrowSvg()
  {
    return PaleoStandardElements::northArrowSvgPath();
  }
} // namespace

QgsPrintLayout *buildHorizonMapLayout( QgisLayerService *layers, QgisProjectService *projectSvc,
                                       const QString &horizon, QString *error )
{
  const auto fail = [error]( const QString &msg ) -> QgsPrintLayout * {
    if ( error )
      *error = msg;
    return nullptr;
  };
  if ( !layers )
    return fail( QObject::tr( "未绑定图层服务" ) );
  QgsProject *project = projectSvc ? projectSvc->project() : nullptr;
  if ( !project )
    return fail( QObject::tr( "未绑定工程服务" ) );

  // 厚度栅格是本图主体（§162：「图内仍含井位和厚度栅格」）；相多边形只在
  // 已经跑过 paleo_facies_polygonize 之后才进图；井位点层可选（链路产物）。
  const QString thicknessId = QStringLiteral( "factor.%1.idw" ).arg( horizon );
  const QString wellsId = QStringLiteral( "wells.thickness.%1" ).arg( horizon );
  const QString faciesId = QStringLiteral( "facies.%1" ).arg( horizon );

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
    return fail( manifestErr.isEmpty() ? QObject::tr( "无法读取图层清单" ) : manifestErr );

  QString thicknessSource, horizonRasterSource;
  bool hasFacies = false;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.horizon != horizon )
      continue;
    if ( d.layerId == thicknessId )
      thicknessSource = d.source;
    else if ( d.layerId == faciesId )
      hasFacies = true;
    else if ( d.layerId.startsWith( QLatin1String( "horizon." ) ) &&
              d.type.compare( QLatin1String( "raster" ), Qt::CaseInsensitive ) == 0 )
      horizonRasterSource = d.source;
  }
  if ( thicknessSource.isEmpty() )
    return fail( QObject::tr( "层位 %1 还没有厚度栅格（先运行编图链）" ).arg( horizon ) );

  QgsMapLayer *thickness = layers->instantiate( thicknessId, error );
  if ( !thickness )
    return fail( error && !error->isEmpty()
                     ? *error
                     : QObject::tr( "无法实例化 %1" ).arg( thicknessId ) );
  hideNodata( thickness ); // nodata 不画

  // 图层表序：index 0 在最上（沿用既有 prepend 惯例）。
  QList<QgsMapLayer *> mapLayers;
  mapLayers.append( thickness );
  if ( hasFacies )
    if ( QgsMapLayer *facies = layers->instantiate( faciesId ) )
      mapLayers.prepend( facies ); // 相面压在厚度栅格之上
  if ( QgsMapLayer *wells = layers->instantiate( wellsId ) )
  {
    enableWellNameLabels( wells ); // 图上有井名
    mapLayers.prepend( wells );    // 井位压在所有图层之上
  }
  for (const auto &id : {QStringLiteral("basemap.topo"), QStringLiteral("basemap.hillshade")})
    if (auto *base = layers->layer(id)) mapLayers.append(base);

  QgsRectangle extent = rasterExtent( thicknessSource );
  if ( extent.isEmpty() )
    extent = rasterExtent( horizonRasterSource );
  if ( extent.isEmpty() )
    extent = thickness->extent();
  try {
    QgsCoordinateTransform transform(thickness->crs(), project->crs(), project->transformContext());
    if (!transform.isValid()) return fail(QObject::tr("无法将图件范围转换到工程地图坐标系"));
    extent = transform.transformBoundingBox(extent);
  } catch (const QgsCsException &ex) { return fail(ex.what()); }

  // A4 横版 + 地图项 + 标题 + 图例 + 比例尺 + 指北针 + CRS 说明。
  auto *layout = new QgsPrintLayout( project );
  layout->initializeDefaults();
  layout->setName( QStringLiteral( "%1_map" ).arg( horizon ) );
  // initializeDefaults 的默认页是竖版 A4 —— 本图是横版，显式改。
  if ( QgsLayoutItemPage *page = layout->pageCollection()->page( 0 ) )
    page->setPageSize( QStringLiteral( "A4" ), QgsLayoutItemPage::Landscape );

  auto *map = new QgsLayoutItemMap( layout );
  map->setId( QStringLiteral( "map" ) );
  map->setLayers( mapLayers );
  map->setCrs(project->crs());
  map->setExtent( extent );
  layout->addLayoutItem( map );
  map->attemptResize( QgsLayoutSize( 277, 165, Qgis::LayoutUnit::Millimeters ) );
  map->attemptMove( QgsLayoutPoint( 10, 22, Qgis::LayoutUnit::Millimeters ) );

  auto *title = new QgsLayoutItemLabel( layout );
  title->setId( QStringLiteral( "title" ) );
  title->setText( QObject::tr( "%1 厚度" ).arg( horizon ) );
  title->setTextFormat( labelFormat( QStringLiteral( "Noto Sans CJK SC" ), 15 ) ); // DESIGN.md 图件标题
  layout->addLayoutItem( title );
  title->attemptResize( QgsLayoutSize( 200, 12, Qgis::LayoutUnit::Millimeters ) );
  title->attemptMove( QgsLayoutPoint( 10, 6, Qgis::LayoutUnit::Millimeters ) );

  // 米制图例（图例右上）：只列本图用到的图层。
  auto *legend = new QgsLayoutItemLegend( layout );
  legend->setId( QStringLiteral( "legend" ) );
  legend->setTitle( QObject::tr( "厚度（米）" ) );
  legend->setLinkedMap( map );
  legend->setSyncMode( Qgis::LegendSyncMode::VisibleLayers );
  legend->setLegendFilterByMapEnabled( true );
  layout->addLayoutItem( legend );
  legend->attemptResize( QgsLayoutSize( 55, 40, Qgis::LayoutUnit::Millimeters ) );
  legend->attemptMove( QgsLayoutPoint( 230, 26, Qgis::LayoutUnit::Millimeters ) );

  // 比例尺（左下），单位为米。
  auto *scalebar = new QgsLayoutItemScaleBar( layout );
  scalebar->setId( QStringLiteral( "scalebar" ) );
  scalebar->setLinkedMap( map );
  scalebar->setUnits( Qgis::DistanceUnit::Meters );
  scalebar->setUnitLabel( QStringLiteral( "m" ) );
  scalebar->applyDefaultSettings();
  scalebar->applyDefaultSize( Qgis::DistanceUnit::Meters );
  layout->addLayoutItem( scalebar );
  scalebar->attemptMove( QgsLayoutPoint( 14, 176, Qgis::LayoutUnit::Millimeters ) );
  scalebar->update();

  // 指北针（左上）：QGIS 自带 SVG 箭头；找不到图时退化为「N」标注。
  const QString arrowSvg = northArrowSvg();
  if ( !arrowSvg.isEmpty() )
  {
    auto *arrow = new QgsLayoutItemPicture( layout );
    arrow->setId( QStringLiteral( "northArrow" ) );
    arrow->setMode( Qgis::PictureFormat::SVG );
    arrow->setPicturePath( arrowSvg );
    arrow->setLinkedMap( map ); // 随地图旋转
    layout->addLayoutItem( arrow );
    arrow->attemptResize( QgsLayoutSize( 12, 12, Qgis::LayoutUnit::Millimeters ) );
    arrow->attemptMove( QgsLayoutPoint( 13, 25, Qgis::LayoutUnit::Millimeters ) );
  }
  else
  {
    auto *arrow = new QgsLayoutItemLabel( layout );
    arrow->setId( QStringLiteral( "northArrow" ) );
    arrow->setText( QStringLiteral( "N" ) );
    arrow->setTextFormat( labelFormat( QStringLiteral( "Noto Sans CJK SC" ), 12 ) );
    layout->addLayoutItem( arrow );
    arrow->attemptResize( QgsLayoutSize( 8, 8, Qgis::LayoutUnit::Millimeters ) );
    arrow->attemptMove( QgsLayoutPoint( 13, 25, Qgis::LayoutUnit::Millimeters ) );
  }

  // CRS 说明（页脚）：工程坐标 · 米 · 未投影（§227 — PDF 上印同一句话）。
  auto *crs = new QgsLayoutItemLabel( layout );
  crs->setId( QStringLiteral( "crsCaption" ) );
  crs->setText(project->crs().type() == Qgis::CrsType::Engineering ? QObject::tr("工程坐标 · 米 · 未投影")
                                                              : project->crs().userFriendlyIdentifier());
  crs->setTextFormat( labelFormat( QStringLiteral( "Noto Sans CJK SC" ), 9 ) );
  layout->addLayoutItem( crs );
  crs->attemptResize( QgsLayoutSize( 140, 8, Qgis::LayoutUnit::Millimeters ) );
  crs->attemptMove( QgsLayoutPoint( 10, 192, Qgis::LayoutUnit::Millimeters ) );

  QStringList sources;
  for (auto *layer : mapLayers) {
    const auto source = layer->customProperty("paleoBasemapAttribution").toString();
    if (!source.isEmpty() && !sources.contains(source)) sources << source;
  }
  if (!sources.isEmpty()) {
    auto *attribution = new QgsLayoutItemLabel(layout);
    attribution->setId(QStringLiteral("basemapAttribution"));
    attribution->setText(sources.join(QStringLiteral(" · ")));
    attribution->setTextFormat(labelFormat(QStringLiteral("Noto Sans CJK SC"), 7));
    layout->addLayoutItem(attribution);
    attribution->attemptResize(QgsLayoutSize(277, 8, Qgis::LayoutUnit::Millimeters));
    attribution->attemptMove(QgsLayoutPoint(10, 200, Qgis::LayoutUnit::Millimeters));
  }

  return layout;
}

} // namespace PaleoLayoutExport
