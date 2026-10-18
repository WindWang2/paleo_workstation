// 层：QGIS 封装
#include "layoutexport.h"
#include "layoutpreview.h"
#include "standardelements.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImageWriter>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSaveFile>
#include <QVBoxLayout>

#include <memory>

#include <gdal.h>

#include <qgis.h>
#include <qgslayoutview.h>

#include <qgsapplication.h>
#include <qgsabstractgeopdfexporter.h>
#include <qgsfillsymbol.h>
#include <qgscoordinatetransform.h>
#include <qgslayertree.h>
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
#include <qgsmaplayerfactory.h>
#include <qgsmapthemecollection.h>
#include <qgspallabeling.h>
#include <qgspathresolver.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>
#include <qgsrastertransparency.h>
#include <qgsreadwritecontext.h>
#include <qgsrectangle.h>
#include <qgstextformat.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerfeatureiterator.h>
#include <qgsvectorlayerlabeling.h>

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
      case PaleoLayoutExport::Format::Tiff: ext = "tif"; break;
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

QString formatExtension(Format format) {
  switch (format) { case Format::Png: return "png"; case Format::Pdf: return "pdf";
    case Format::Svg: return "svg"; case Format::Tiff: return "tif"; }
  return {};
}
bool formatAvailable(Format format) {
  if (format != Format::Tiff) return true;
  const auto supported = QImageWriter::supportedImageFormats();
  return supported.contains("tiff") || supported.contains("tif");
}
bool geoPdfAvailable(QString *reason) {
  const bool available = QgsAbstractGeospatialPdfExporter::geospatialPDFCreationAvailable();
  if (!available && reason) *reason = QgsAbstractGeospatialPdfExporter::geospatialPDFAvailabilityExplanation();
  return available;
}

ExportOutcome exportLayout( QgsPrintLayout *layout, const QString &outPath, Format format,
                            double dpi, const PageRange &range, const ExportOptions &options )
{
  return exportLayout( static_cast<QgsLayout *>( layout ), outPath, format, dpi, range, options );
}

ExportOutcome exportLayout( QgsLayout *layout, const QString &outPath, Format format,
                            double dpi, const PageRange &range, const ExportOptions &options )
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

  if (!formatAvailable(format)) { outcome.error = QObject::tr("当前运行环境缺少 TIFF 图像编码器"); return outcome; }
  if (options.worldFile && (format == Format::Png || format == Format::Tiff) &&
      (!layout->referenceMap() || !layout->referenceMap()->crs().isValid())) {
    outcome.error = QObject::tr("坐标定位文件需要有效的参考地图和坐标系"); return outcome;
  }
  if (options.geoPdf && format == Format::Pdf) {
    if (!geoPdfAvailable(&outcome.error)) return outcome;
    auto *reference = layout->referenceMap();
    if (!reference || !reference->crs().isValid() || reference->crs().type() == Qgis::CrsType::Engineering) {
      outcome.error = QObject::tr("GeoPDF 需要已配准的地理或投影坐标系"); return outcome;
    }
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

  std::unique_ptr<QgsLayout> transparent;
  if (options.transparentBackground && (format == Format::Png || format == Format::Tiff)) {
    transparent = trimmedClone(layout, pages);
    if (!transparent) { outcome.error = QObject::tr("无法创建透明图件快照"); return outcome; }
    const auto pageStyle = QgsFillSymbol::createSimple({{"color", "255,255,255,0"}, {"outline_style", "no"}});
    transparent->pageCollection()->setPageStyleSymbol(pageStyle.get());
    layout = transparent.get();
    pages.clear();
    for (int i = 0; i < layout->pageCollection()->pageCount(); ++i) pages << i;
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
    case Format::Tiff:
    {
      if ( contiguousFromZero )
      {
        QgsLayoutExporter exporter( layout );
        QgsLayoutExporter::ImageExportSettings settings;
        settings.generateWorldFile = options.worldFile;
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
        settings.generateWorldFile = options.worldFile;
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
        settings.writeGeoPdf = options.geoPdf;
        settings.textRenderFormat = options.textAsPaths ? Qgis::TextRenderFormat::AlwaysOutlines : Qgis::TextRenderFormat::AlwaysText;
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
        settings.writeGeoPdf = options.geoPdf;
        settings.textRenderFormat = options.textAsPaths ? Qgis::TextRenderFormat::AlwaysOutlines : Qgis::TextRenderFormat::AlwaysText;
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
        settings.textRenderFormat = options.textAsPaths ? Qgis::TextRenderFormat::AlwaysOutlines : Qgis::TextRenderFormat::AlwaysText;
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
        settings.textRenderFormat = options.textAsPaths ? Qgis::TextRenderFormat::AlwaysOutlines : Qgis::TextRenderFormat::AlwaysText;
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
    if (options.worldFile && (format == Format::Png || format == Format::Tiff)) {
      for (const auto &image : outcome.files) {
        const QFileInfo info(image);
        const auto base = info.absoluteDir().filePath(info.completeBaseName());
        for (const auto &extension : QStringList{".pgw", ".tfw", ".wld"})
          if (QFile::exists(base + extension)) outcome.auxiliaryFiles << base + extension;
        if (auto *reference = layout->referenceMap(); reference && reference->crs().isValid()) {
          QSaveFile crs(base + ".prj"); const auto bytes = reference->crs().toWkt().toUtf8();
          if (!crs.open(QIODevice::WriteOnly) || crs.write(bytes) != bytes.size() || !crs.commit()) {
            outcome.ok = false; outcome.error = QObject::tr("坐标系配套文件写入失败"); return outcome;
          }
          outcome.auxiliaryFiles << base + ".prj";
        }
      }
    }
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

  // 瞬时布局的活图层样式守护（opt-05）：构建厚度图时为图面效果改写「活」
  // 工程图层（厚度栅格 nodata 透明表、井位图层的井名标注）——布局地图项按
  // 活图层渲染，渲染期间改动必须在场；但布局是瞬时的（调用方导出后即删），
  // 改动不得比布局活得久，否则画布上残留「出图专用」的透明表/标注并随
  // .qgz 持久化。用法：改前 watch* 快照入场状态，守护挂为布局子对象——
  // 布局析构即逐个恢复；布局建不出来（unique_ptr 提前析构）同样恢复。
  // 图层先亡 → QPointer 防悬垂，不复活已删图层。
  class TransientLayerStyleGuard : public QObject
  {
  public:
    using QObject::QObject;

    void watchRaster( QgsRasterLayer *layer )
    {
      if ( !layer || !layer->renderer() )
        return;
      RasterState s;
      s.layer = layer;
      if ( const QgsRasterTransparency *t = layer->renderer()->rasterTransparency() )
      {
        s.transparency = *t; // 值拷贝入场快照（透明表在渲染器上）
        s.hadTransparency = true;
      }
      m_rasters.append( s );
    }

    void watchVector( QgsVectorLayer *layer )
    {
      if ( !layer )
        return;
      VectorState s;
      s.layer = layer;
      s.labeling = layer->labeling() ? layer->labeling()->clone() : nullptr;
      s.labelsEnabled = layer->labelsEnabled();
      m_vectors.append( s );
    }

    ~TransientLayerStyleGuard() override
    {
      for ( const RasterState &s : m_rasters )
        if ( s.layer && s.layer->renderer() )
          s.layer->renderer()->setRasterTransparency(
              s.hadTransparency ? new QgsRasterTransparency( s.transparency ) : nullptr );
      for ( const VectorState &s : m_vectors )
      {
        if ( s.layer )
        {
          s.layer->setLabeling( s.labeling ? s.labeling->clone() : nullptr );
          s.layer->setLabelsEnabled( s.labelsEnabled );
        }
        delete s.labeling; // 快照本体随守护析构（还原喂的是克隆，图层接管克隆体）
      }
    }

  private:
    struct RasterState
    {
      QPointer<QgsRasterLayer> layer;
      QgsRasterTransparency transparency;
      bool hadTransparency = false;
    };
    struct VectorState
    {
      QPointer<QgsVectorLayer> layer;
      QgsAbstractVectorLayerLabeling *labeling = nullptr; // 入场快照（守护持有）
      bool labelsEnabled = false;
    };
    QVector<RasterState> m_rasters;
    QVector<VectorState> m_vectors;
  };

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
    Q_UNUSED( family ); // 字体族由工厂统一（vendored "Noto Sans SC" 优先链）
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

  // 出图专用改写的入场快照（opt-05）：守护挂为布局子对象，布局析构即把
  // 透明表/标注还原；布局建不出来（此处提前 return）也照样还原。
  std::unique_ptr<TransientLayerStyleGuard> liveStyleGuard( new TransientLayerStyleGuard );
  if ( auto *thicknessRaster = qobject_cast<QgsRasterLayer *>( thickness ) )
    liveStyleGuard->watchRaster( thicknessRaster );
  hideNodata( thickness ); // nodata 不画

  // 图层表序：index 0 在最上（沿用既有 prepend 惯例）。
  QList<QgsMapLayer *> mapLayers;
  mapLayers.append( thickness );
  if ( hasFacies )
    if ( QgsMapLayer *facies = layers->instantiate( faciesId ) )
      mapLayers.prepend( facies ); // 相面压在厚度栅格之上
  if ( QgsMapLayer *wells = layers->instantiate( wellsId ) )
  {
    if ( auto *wellsVector = qobject_cast<QgsVectorLayer *>( wells ) )
      liveStyleGuard->watchVector( wellsVector ); // 标注改写前先快照
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
  // 两侧都有真实 CRS 才做坐标转换；工程坐标（未投影）下范围即图面坐标。
  if ( thickness->crs().isValid() && project->crs().isValid() &&
       thickness->crs() != project->crs() ) {
    try {
      QgsCoordinateTransform transform(thickness->crs(), project->crs(), project->transformContext());
      if (!transform.isValid()) return fail(QObject::tr("无法将图件范围转换到工程地图坐标系"));
      extent = transform.transformBoundingBox(extent);
    } catch (const QgsCsException &ex) { return fail(ex.what()); }
  }

  // A4 横版 + 地图项 + 标题 + 图例 + 比例尺 + 指北针 + CRS 说明。
  auto *layout = new QgsPrintLayout( project );
  liveStyleGuard->setParent( layout ); // 活图层样式随瞬时布局存亡（opt-05）
  liveStyleGuard.release();
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

  // 图签（TODOS P3「标准导出流图签块」，规范图件四要素收口）：编制/审核读
  // 工程 customVariables（paleoAuthor/paleoReviewer——快照导出链保留该面，
  // layoutexport captureProjectSnapshot 随 DOM 走），缺省留空线供手签；日期
  // 取当日。不虚构数据、不加输入 UI；落在 CRS 页脚同行右段，不与任何既有
  // 版面元素重叠（crsCaption 占 x≤150，底图署名行 y≥200）。
  {
    const QVariantMap projectVars = project->customVariables();
    const QString blank = QStringLiteral( "________" );
    const QString author =
        projectVars.value( QStringLiteral( "paleoAuthor" ) ).toString();
    const QString reviewer =
        projectVars.value( QStringLiteral( "paleoReviewer" ) ).toString();
    auto *signature = new QgsLayoutItemLabel( layout );
    signature->setId( QStringLiteral( "signatureBlock" ) );
    signature->setText( QObject::tr( "编制：%1　　审核：%2\n日期：%3" )
                            .arg( author.isEmpty() ? blank : author,
                                  reviewer.isEmpty() ? blank : reviewer,
                                  QDate::currentDate().toString( Qt::ISODate ) ) );
    signature->setTextFormat( labelFormat( QStringLiteral( "Noto Sans CJK SC" ), 8 ) );
    layout->addLayoutItem( signature );
    signature->attemptResize( QgsLayoutSize( 130, 10, Qgis::LayoutUnit::Millimeters ) );
    signature->attemptMove( QgsLayoutPoint( 157, 190, Qgis::LayoutUnit::Millimeters ) );
  }

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

LayoutHandle::LayoutHandle(std::unique_ptr<QgsPrintLayout> layout) : m_layout(std::move(layout)) {}
LayoutHandle::~LayoutHandle() = default;

ProjectHandle::ProjectHandle(std::unique_ptr<QgsProject> project) : m_project(std::move(project)) {}
ProjectHandle::~ProjectHandle() = default;

QString qgisVersion() { return Qgis::version(); }

bool captureLayout(QgsPrintLayout *layout, LayoutSnapshot &out, QString *error) {
  const auto fail = [error](const QString &why) {
    if (error)
      *error = why;
    return false;
  };
  if (!layout)
    return fail(QObject::tr("图件版面缺失"));
  if (!captureProjectSnapshot(layout->project(), out.project, error))
    return false;
  QgsReadWriteContext context;
  out.xml.appendChild(layout->writeXml(out.xml, context));
  // 图面引用的图层 id（去重，保序）：worker 侧据此核对数据源可用性，并按同
  // 一顺序导出所选图层的成果数据。
  for (auto *item : layout->items())
    if (auto *map = dynamic_cast<QgsLayoutItemMap *>(item))
      for (auto *layer : map->layers())
        if (layer && !out.requiredLayerIds.contains(layer->id()))
          out.requiredLayerIds << layer->id();
  return true;
}

std::unique_ptr<LayoutHandle> rebuildLayout(QgsProject *project, const QDomDocument &xml,
                                            QString *error) {
  if (!project) {
    if (error)
      *error = QObject::tr("图件工程快照缺失");
    return nullptr;
  }
  std::unique_ptr<QgsPrintLayout> layout(new QgsPrintLayout(project));
  QgsReadWriteContext context;
  if (!layout->readXml(xml.documentElement(), xml, context)) {
    if (error)
      *error = QObject::tr("无法重建图件版面");
    return nullptr;
  }
  return std::make_unique<LayoutHandle>(std::move(layout));
}

bool validateLayoutLayers(QgsPrintLayout *layout, const QStringList &requiredLayerIds, QString *error) {
  if (!layout)
    return false;
  for (const auto &id : requiredLayerIds) {
    auto *layer = layout->project()->mapLayer(id);
    if (!layer || !layer->isValid()) {
      if (error)
        *error = QObject::tr("所选制图图层数据源不可用：%1").arg(id);
      return false;
    }
  }
  return true;
}

bool saveLayoutTemplate(QgsPrintLayout *layout, const QString &path, QString *error) {
  if (!layout) {
    if (error)
      *error = QObject::tr("图件版面缺失");
    return false;
  }
  QgsReadWriteContext context;
  if (!layout->saveAsTemplate(path, context)) {
    if (error)
      *error = QObject::tr("无法写出版面模板");
    return false;
  }
  return true;
}

} // namespace PaleoLayoutExport

PaleoLayoutPreview::PaleoLayoutPreview(QWidget *parent) : QWidget(parent) {
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  m_view = new QgsLayoutView(this);
  outer->addWidget(m_view);
}
PaleoLayoutPreview::~PaleoLayoutPreview() = default;
void PaleoLayoutPreview::setCurrentLayout(QgsPrintLayout *layout) { m_view->setCurrentLayout(layout); }
void PaleoLayoutPreview::zoomFull() { m_view->zoomFull(); }

namespace {
const QString kSnapRoot = QStringLiteral("qgis");
const QString kSnapLayers = QStringLiteral("projectlayers");
const QString kSnapTree = QStringLiteral("layertree");

QgsReadWriteContext absolutePathContext() {
  QgsReadWriteContext context;
  context.setPathResolver(QgsPathResolver()); // 空 base：路径原样（绝对）读写
  return context;
}
} // namespace

bool PaleoLayoutExport::captureProjectSnapshot(const QgsProject *project,
                                               ProjectSnapshot &out,
                                               QString *reason) {
  auto fail = [reason](const QString &why) {
    if (reason)
      *reason = why;
    return false;
  };
  out = ProjectSnapshot();
  QDomDocument doc(kSnapRoot);
  QDomElement root = doc.createElement(kSnapRoot);
  doc.appendChild(root);
  if (!project) {
    out.xml = doc;
    return true; // 无工程：版面只含非地图项，空私有工程即可
  }

  const QgsReadWriteContext context = absolutePathContext();
  QDomElement layersEl = doc.createElement(kSnapLayers);
  root.appendChild(layersEl);
  const QMap<QString, QgsMapLayer *> layers = project->mapLayers();
  for (auto it = layers.cbegin(); it != layers.cend(); ++it) {
    QgsMapLayer *layer = it.value();
    if (!layer)
      continue;
    if (layer->type() == Qgis::LayerType::Plugin)
      return fail(QObject::tr("图层「%1」是插件图层，无法在后台重建")
                      .arg(layer->name()));
    auto *vector = qobject_cast<QgsVectorLayer *>(layer);
    if (vector && vector->isEditable() && vector->isModified())
      return fail(QObject::tr("图层「%1」有未提交的编辑").arg(layer->name()));
    QDomElement layerEl = doc.createElement(QStringLiteral("maplayer"));
    if (!layer->writeLayerXml(layerEl, doc, context))
      return fail(QObject::tr("图层「%1」序列化失败").arg(layer->name()));
    layersEl.appendChild(layerEl);
    // memory 图层的 XML 只有字段定义没有要素：要素在 GUI 线程拷出（值类型，
    // 隐式共享只读），worker 重建后灌回私有图层。
    if (vector && vector->providerType() == QLatin1String("memory")) {
      out.memorySources.insert(
          layer->id(), std::make_shared<QgsVectorLayerFeatureSource>(vector));
    }
  }

  QDomElement treeEl = doc.createElement(kSnapTree);
  root.appendChild(treeEl);
  if (QgsLayerTree *tree = const_cast<QgsProject *>(project)->layerTreeRoot())
    tree->writeXml(treeEl, context);
  if (QgsMapThemeCollection *themes =
          const_cast<QgsProject *>(project)->mapThemeCollection())
    themes->writeXml(doc); // 写进 <qgis><visibility-presets>

  out.xml = doc;
  out.crs = project->crs();
  out.transformContext = project->transformContext();
  out.ellipsoid = project->ellipsoid();
  out.title = project->title();
  out.homePath = project->homePath();
  out.customVariables = project->customVariables();
  return true;
}

std::unique_ptr<QgsProject>
PaleoLayoutExport::rebuildProject(const ProjectSnapshot &snapshot,
                                  QString *error, QStringList *warnings) {
  auto project = std::make_unique<QgsProject>();
  project->setCrs(snapshot.crs);
  project->setTransformContext(snapshot.transformContext);
  if (!snapshot.ellipsoid.isEmpty())
    project->setEllipsoid(snapshot.ellipsoid);
  project->setTitle(snapshot.title);
  if (!snapshot.homePath.isEmpty())
    project->setPresetHomePath(snapshot.homePath);
  project->setCustomVariables(snapshot.customVariables);

  QgsReadWriteContext context = absolutePathContext();
  context.setTransformContext(snapshot.transformContext);
  const QDomElement root = snapshot.xml.documentElement();

  QList<QgsMapLayer *> layers;
  const QDomElement layersEl = root.firstChildElement(kSnapLayers);
  for (QDomElement layerEl =
           layersEl.firstChildElement(QStringLiteral("maplayer"));
       !layerEl.isNull();
       layerEl = layerEl.nextSiblingElement(QStringLiteral("maplayer"))) {
    bool typeOk = false;
    const Qgis::LayerType type = QgsMapLayerFactory::typeFromString(
        layerEl.attribute(QStringLiteral("type")), typeOk);
    if (!typeOk)
      continue;
    QgsMapLayerFactory::LayerOptions options(snapshot.transformContext);
    options.loadDefaultStyle = false;
    const QString provider =
        layerEl.firstChildElement(QStringLiteral("provider")).text();
    std::unique_ptr<QgsMapLayer> layer(QgsMapLayerFactory::createLayer(
        QString(), QString(), type, options, provider));
    if (!layer)
      continue;
    // readLayerXml 失败（XML 缺字段）或数据源离线（layer invalid）仍保留该
    // 图层：与活工程里「无效图层」同口径，地图项照常跳过它，而不是整次导出
    // 失败。但导出产物少层必须可核实——逐层收集 warning（调用方呈现）。
    // memory 层无持久源、要素在下方灌回，不参与有效性判断。
    const bool readOk = layer->readLayerXml(layerEl, context);
    const bool memoryBacked = snapshot.memorySources.contains(layer->id());
    if ((!readOk || !layer->isValid()) && !memoryBacked && warnings) {
      QString name = layer->name();
      if (name.isEmpty()) {
        name = layerEl.attribute(QStringLiteral("title"));
        if (name.isEmpty()) {
          const QDomElement layerName =
              layerEl.firstChildElement(QStringLiteral("layername"));
          if (!layerName.isNull())
            name = layerName.text();
        }
      }
      warnings->push_back(QObject::tr("%1：数据源无法读取，导出图中不含该图层")
                              .arg(name.isEmpty() ? QObject::tr("未命名图层") : name));
    }
    if (auto *vector = qobject_cast<QgsVectorLayer *>(layer.get())) {
      const auto source = snapshot.memorySources.value(vector->id());
      if (source && vector->dataProvider()) {
        QgsFeatureList batch;
        QgsFeature feature;
        auto iterator = source->getFeatures(QgsFeatureRequest());
        while (iterator.nextFeature(feature)) {
          batch.append(feature);
          if (batch.size() >= 1024) {
            vector->dataProvider()->addFeatures(batch);
            batch.clear();
          }
        }
        if (!batch.isEmpty())
          vector->dataProvider()->addFeatures(batch);
        vector->updateExtents();
      }
    }
    layers.append(layer.release());
  }
  if (!layers.isEmpty() &&
      project->addMapLayers(layers, false).size() != layers.size()) {
    if (error)
      *error = QObject::tr("私有工程装载图层失败");
    return nullptr;
  }
  for (QgsMapLayer *layer : std::as_const(layers))
    layer->resolveReferences(project.get());

  const QDomElement treeGroup =
      root.firstChildElement(kSnapTree).firstChildElement(
          QStringLiteral("layer-tree-group"));
  if (!treeGroup.isNull()) {
    QgsLayerTree *tree = project->layerTreeRoot();
    tree->readChildrenFromXml(treeGroup, context);
    tree->resolveReferences(project.get());
    const QDomElement order =
        treeGroup.firstChildElement(QStringLiteral("custom-order"));
    if (!order.isNull()) {
      QStringList ids;
      for (QDomElement item = order.firstChildElement(QStringLiteral("item"));
           !item.isNull();
           item = item.nextSiblingElement(QStringLiteral("item")))
        ids << item.text();
      tree->setCustomLayerOrder(ids);
      tree->setHasCustomLayerOrder(
          order.attribute(QStringLiteral("enabled")).toInt() != 0);
    }
  }
  project->mapThemeCollection()->readXml(snapshot.xml);
  return project;
}

std::unique_ptr<PaleoLayoutExport::ProjectHandle>
PaleoLayoutExport::rebuildProjectHandle(const ProjectSnapshot &snapshot,
                                        QString *error, QStringList *warnings) {
  return std::make_unique<PaleoLayoutExport::ProjectHandle>(
      rebuildProject(snapshot, error, warnings));
}
