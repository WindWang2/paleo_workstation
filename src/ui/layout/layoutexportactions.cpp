#include "layoutexportactions.h"

#include <QAction>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSet>
#include <QSpinBox>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

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
  QString withDefaultExtension( const QString &path, PaleoLayoutExportActions::Format format )
  {
    const char *ext = nullptr;
    switch ( format )
    {
      case PaleoLayoutExportActions::Format::Png: ext = "png"; break;
      case PaleoLayoutExportActions::Format::Pdf: ext = "pdf"; break;
      case PaleoLayoutExportActions::Format::Svg: ext = "svg"; break;
    }
    const QFileInfo info( path );
    if ( info.suffix().compare( QLatin1String( ext ), Qt::CaseInsensitive ) == 0 )
      return path;
    return path + QLatin1Char( '.' ) + QLatin1String( ext );
  }

  QString fileDialogFilter( PaleoLayoutExportActions::Format format )
  {
    switch ( format )
    {
      case PaleoLayoutExportActions::Format::Png:
        return QObject::tr( "PNG image (*.png)" );
      case PaleoLayoutExportActions::Format::Pdf:
        return QObject::tr( "PDF document (*.pdf)" );
      case PaleoLayoutExportActions::Format::Svg:
        return QObject::tr( "SVG document (*.svg)" );
    }
    return QString();
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

// ---------------------------------------------------------------------------
// Export settings dialog — native controls, DESIGN.md: no custom chrome.
// ---------------------------------------------------------------------------
namespace
{
  class ExportSettingsDialog : public QDialog
  {
    public:
      ExportSettingsDialog( int pageCount, int currentPage0, QWidget *parent )
        : QDialog( parent )
        , m_currentPage0( currentPage0 )
      {
        setWindowTitle( tr( "Export Layout" ) );

        m_dpiSpin = new QSpinBox( this );
        m_dpiSpin->setRange( 72, 1200 );
        m_dpiSpin->setValue( 300 );
        m_dpiSpin->setSuffix( tr( " dpi" ) );

        auto *form = new QFormLayout;
        form->addRow( tr( "Resolution" ), m_dpiSpin );

        const bool multiPage = pageCount > 1;
        QRadioButton *allRadio = new QRadioButton( tr( "All pages" ), this );
        m_currentRadio = new QRadioButton( tr( "Current page (%1)" ).arg( currentPage0 + 1 ), this );
        m_rangeRadio = new QRadioButton( tr( "Pages from" ), this );
        allRadio->setChecked( true );

        m_fromSpin = new QSpinBox( this );
        m_toSpin = new QSpinBox( this );
        m_fromSpin->setRange( 1, qMax( 1, pageCount ) );
        m_toSpin->setRange( 1, qMax( 1, pageCount ) );
        m_fromSpin->setValue( 1 );
        m_toSpin->setValue( qMax( 1, pageCount ) );
        auto *rangeRow = new QHBoxLayout;
        rangeRow->addWidget( m_fromSpin );
        rangeRow->addWidget( new QLabel( tr( "to" ), this ) );
        rangeRow->addWidget( m_toSpin );
        rangeRow->addStretch();

        auto *rangeBox = new QGroupBox( tr( "Page Range" ), this );
        auto *rangeLay = new QVBoxLayout( rangeBox );
        rangeLay->addWidget( allRadio );
        rangeLay->addWidget( m_currentRadio );
        auto *rangeModeRow = new QHBoxLayout;
        rangeModeRow->addWidget( m_rangeRadio );
        rangeModeRow->addLayout( rangeRow );
        rangeLay->addLayout( rangeModeRow );

        if ( !multiPage )
        {
          m_currentRadio->setEnabled( false );
          m_rangeRadio->setEnabled( false );
          m_fromSpin->setEnabled( false );
          m_toSpin->setEnabled( false );
        }
        else
        {
          auto *group = new QButtonGroup( this );
          group->addButton( allRadio );
          group->addButton( m_currentRadio );
          group->addButton( m_rangeRadio );
          connect( m_rangeRadio, &QRadioButton::toggled, m_fromSpin, &QSpinBox::setEnabled );
          connect( m_rangeRadio, &QRadioButton::toggled, m_toSpin, &QSpinBox::setEnabled );
          m_fromSpin->setEnabled( false );
          m_toSpin->setEnabled( false );
        }

        auto *buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
        connect( buttons, &QDialogButtonBox::accepted, this, &QDialog::accept );
        connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );

        auto *root = new QVBoxLayout( this );
        root->addLayout( form );
        root->addWidget( rangeBox );
        root->addWidget( buttons );
      }

      double dpi() const { return static_cast<double>( m_dpiSpin->value() ); }

      PaleoLayoutExportActions::PageRange pageRange() const
      {
        PaleoLayoutExportActions::PageRange range;
        if ( m_currentRadio->isChecked() )
        {
          range.mode = PaleoLayoutExportActions::PageRange::Mode::Current;
          range.currentPage = m_currentPage0;
        }
        else if ( m_rangeRadio->isChecked() )
        {
          range.mode = PaleoLayoutExportActions::PageRange::Mode::Range;
          range.fromPage = m_fromSpin->value() - 1; // dialog is 1-based for users
          range.toPage = m_toSpin->value() - 1;
        }
        return range;
      }

    private:
      QSpinBox *m_dpiSpin = nullptr;
      int m_currentPage0 = 0;
      QRadioButton *m_currentRadio = nullptr;
      QRadioButton *m_rangeRadio = nullptr;
      QSpinBox *m_fromSpin = nullptr;
      QSpinBox *m_toSpin = nullptr;
  };
}

// ---------------------------------------------------------------------------
// PaleoLayoutExportActions
// ---------------------------------------------------------------------------

PaleoLayoutExportActions::PaleoLayoutExportActions( QObject *parent )
  : QObject( parent )
{
  m_pngAction = new QAction( tr( "Export as &PNG…" ), this );
  m_pngAction->setObjectName( QStringLiteral( "actionExportLayoutPng" ) );
  connect( m_pngAction, &QAction::triggered, this, [this] { runExportUi( Format::Png ); } );

  m_pdfAction = new QAction( tr( "Export as &PDF…" ), this );
  m_pdfAction->setObjectName( QStringLiteral( "actionExportLayoutPdf" ) );
  connect( m_pdfAction, &QAction::triggered, this, [this] { runExportUi( Format::Pdf ); } );

  m_svgAction = new QAction( tr( "Export as &SVG…" ), this );
  m_svgAction->setObjectName( QStringLiteral( "actionExportLayoutSvg" ) );
  connect( m_svgAction, &QAction::triggered, this, [this] { runExportUi( Format::Svg ); } );
}

QAction *PaleoLayoutExportActions::exportPngAction() { return m_pngAction; }
QAction *PaleoLayoutExportActions::exportPdfAction() { return m_pdfAction; }
QAction *PaleoLayoutExportActions::exportSvgAction() { return m_svgAction; }

void PaleoLayoutExportActions::setLayoutProvider( const std::function<QgsLayout *()> &provider )
{
  m_layoutProvider = provider;
}

void PaleoLayoutExportActions::setCurrentPageProvider( const std::function<int()> &provider )
{
  m_currentPageProvider = provider;
}

void PaleoLayoutExportActions::setStatusTarget( QStatusBar *status )
{
  m_statusTarget = status;
}

void PaleoLayoutExportActions::setOpenFolderEnabled( bool enabled )
{
  m_openFolderEnabled = enabled;
}

PaleoLayoutExportActions::ExportOutcome
PaleoLayoutExportActions::exportLayout( QgsLayout *layout, const QString &outPath, Format format,
                                        double dpi, const PageRange &range )
{
  ExportOutcome outcome;

  if ( !layout )
  {
    outcome.error = tr( "No layout to export." );
    emit exportFinished( QString(), false );
    return outcome;
  }
  if ( outPath.isEmpty() )
  {
    outcome.error = tr( "No destination file given." );
    emit exportFinished( QString(), false );
    return outcome;
  }

  const QString path = withDefaultExtension( outPath, format );
  const int pageCount = layout->pageCollection() ? layout->pageCollection()->pageCount() : 0;
  if ( pageCount < 1 )
  {
    outcome.error = tr( "The layout has no pages to export." );
    emit exportFinished( path, false );
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
        outcome.error = tr( "The page range selects no pages of this %1-page layout." ).arg( pageCount );
        emit exportFinished( path, false );
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
          outcome.error = tr( "Could not prepare the page selection for export." );
          emit exportFinished( path, false );
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
          outcome.error = tr( "Could not prepare the page selection for export." );
          emit exportFinished( path, false );
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
          outcome.error = tr( "Could not prepare the page selection for export." );
          emit exportFinished( path, false );
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
    outcome.error = extraError.isEmpty() ? tr( "Export failed." ) : extraError;
  }

  emit exportFinished( path, outcome.ok );
  return outcome;
}

void PaleoLayoutExportActions::runExportUi( Format format )
{
  QgsLayout *layout = m_layoutProvider ? m_layoutProvider() : nullptr;
  if ( !layout )
  {
    if ( m_statusTarget )
      m_statusTarget->showMessage( tr( "No layout to export." ), 4000 );
    emit exportFinished( QString(), false );
    return;
  }

  QWidget *dialogParent = qobject_cast<QWidget *>( parent() );
  const QString path = QFileDialog::getSaveFileName( dialogParent, tr( "Export Layout" ),
                                                     QString(), fileDialogFilter( format ) );
  if ( path.isEmpty() )
    return; // user canceled — no export, no signal

  const int pageCount = layout->pageCollection() ? layout->pageCollection()->pageCount() : 0;
  const int currentPage0 = m_currentPageProvider ? qMax( 0, m_currentPageProvider() ) : 0;

  ExportSettingsDialog dialog( pageCount, currentPage0, dialogParent );
  if ( dialog.exec() != QDialog::Accepted )
    return;

  const ExportOutcome outcome = exportLayout( layout, path, format, dialog.dpi(), dialog.pageRange() );

  if ( m_statusTarget )
  {
    if ( outcome.ok )
    {
      m_statusTarget->showMessage( outcome.files.size() > 1
                                     ? tr( "Exported %1 files to %2" ).arg( outcome.files.size() ).arg( QFileInfo( outcome.files.first() ).absolutePath() )
                                     : tr( "Exported %1" ).arg( outcome.files.value( 0 ) ), 4000 );
    }
    else
    {
      m_statusTarget->showMessage( outcome.error, 6000 );
    }
  }

  if ( m_openFolderEnabled && outcome.ok && !outcome.files.isEmpty() )
    QDesktopServices::openUrl( QUrl::fromLocalFile( QFileInfo( outcome.files.first() ).absolutePath() ) );
}
