// 层：QGIS 封装
#include "layoutexport.h"

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QSet>

#include <memory>

#include <qgslayout.h>
#include <qgslayoutexporter.h>
#include <qgslayoutitem.h>
#include <qgslayoutitempage.h>
#include <qgslayoutpagecollection.h>

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
      case QgsLayoutExporter::Canceled: error = QObject::tr( "Export canceled." ); break;
      case QgsLayoutExporter::MemoryError: error = QObject::tr( "Not enough memory to export the layout." ); break;
      case QgsLayoutExporter::FileError:
        error = QObject::tr( "Could not write the export file %1." ).arg( exporter.errorFile() );
        break;
      case QgsLayoutExporter::PrintError: error = QObject::tr( "Could not start printing the export." ); break;
      case QgsLayoutExporter::SvgLayerError: error = QObject::tr( "Could not create the layered SVG file." ); break;
      case QgsLayoutExporter::IteratorError: error = QObject::tr( "Error iterating over the layout." ); break;
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
    outcome.error = QObject::tr( "No layout to export." );
    return outcome;
  }
  if ( outPath.isEmpty() )
  {
    outcome.error = QObject::tr( "No destination file given." );
    return outcome;
  }

  const QString path = withDefaultExtension( outPath, format );
  outcome.effectivePath = path;
  const int pageCount = layout->pageCollection() ? layout->pageCollection()->pageCount() : 0;
  if ( pageCount < 1 )
  {
    outcome.error = QObject::tr( "The layout has no pages to export." );
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
        outcome.error = QObject::tr( "The page range selects no pages of this %1-page layout." ).arg( pageCount );
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
          outcome.error = QObject::tr( "Could not prepare the page selection for export." );
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
          outcome.error = QObject::tr( "Could not prepare the page selection for export." );
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
          outcome.error = QObject::tr( "Could not prepare the page selection for export." );
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
    outcome.error = extraError.isEmpty() ? QObject::tr( "Export failed." ) : extraError;
  }

  return outcome;
}

} // namespace PaleoLayoutExport
