// 层：视图
#include "layoutexportactions.h"

#include <QAction>
#include <QButtonGroup>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressDialog>
#include <QRadioButton>
#include <QSet>
#include <QSpinBox>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include <QDomDocument>
#include <qgslayout.h>
#include <qgslayoutpagecollection.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsreadwritecontext.h>

#include "../../services/paleotaskservice.h"

namespace
{
  QString fileDialogFilter( PaleoLayoutExportActions::Format format )
  {
    switch ( format )
    {
      case PaleoLayoutExportActions::Format::Png:
        return QObject::tr( "PNG 图像 (*.png)" );
      case PaleoLayoutExportActions::Format::Pdf:
        return QObject::tr( "PDF 文档 (*.pdf)" );
      case PaleoLayoutExportActions::Format::Svg:
        return QObject::tr( "SVG 文档 (*.svg)" );
    }
    return QString();
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
        setWindowTitle( tr( "导出版面" ) );

        m_dpiSpin = new QSpinBox( this );
        m_dpiSpin->setRange( 72, 1200 );
        m_dpiSpin->setValue( 300 );
        m_dpiSpin->setSuffix( tr( " dpi" ) );

        auto *form = new QFormLayout;
        form->addRow( tr( "分辨率" ), m_dpiSpin );

        const bool multiPage = pageCount > 1;
        QRadioButton *allRadio = new QRadioButton( tr( "全部页面" ), this );
        m_currentRadio = new QRadioButton( tr( "当前页（第 %1 页）" ).arg( currentPage0 + 1 ), this );
        m_rangeRadio = new QRadioButton( tr( "页面范围 从" ), this );
        allRadio->setChecked( true );

        m_fromSpin = new QSpinBox( this );
        m_toSpin = new QSpinBox( this );
        m_fromSpin->setRange( 1, qMax( 1, pageCount ) );
        m_toSpin->setRange( 1, qMax( 1, pageCount ) );
        m_fromSpin->setValue( 1 );
        m_toSpin->setValue( qMax( 1, pageCount ) );
        auto *rangeRow = new QHBoxLayout;
        rangeRow->addWidget( m_fromSpin );
        rangeRow->addWidget( new QLabel( tr( "到" ), this ) );
        rangeRow->addWidget( m_toSpin );
        rangeRow->addStretch();

        auto *rangeBox = new QGroupBox( tr( "页面范围" ), this );
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
  m_pngAction = new QAction( tr( "导出为 &PNG…" ), this );
  m_pngAction->setObjectName( QStringLiteral( "actionExportLayoutPng" ) );
  connect( m_pngAction, &QAction::triggered, this, [this] { runExportUi( Format::Png ); } );

  m_pdfAction = new QAction( tr( "导出为 &PDF…" ), this );
  m_pdfAction->setObjectName( QStringLiteral( "actionExportLayoutPdf" ) );
  connect( m_pdfAction, &QAction::triggered, this, [this] { runExportUi( Format::Pdf ); } );

  m_svgAction = new QAction( tr( "导出为 &SVG…" ), this );
  m_svgAction->setObjectName( QStringLiteral( "actionExportLayoutSvg" ) );
  connect( m_svgAction, &QAction::triggered, this, [this] { runExportUi( Format::Svg ); } );
}

QAction *PaleoLayoutExportActions::exportPngAction() { return m_pngAction; }
QAction *PaleoLayoutExportActions::exportPdfAction() { return m_pdfAction; }
QAction *PaleoLayoutExportActions::exportSvgAction() { return m_svgAction; }

void PaleoLayoutExportActions::setTaskService( PaleoTaskService *service )
{
  m_taskSvc = service;
}

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

// 逻辑核心在 qgis/layoutexport（QGIS 封装层；workflow/mapexport 也走它）。
// 这里只补 exportFinished 信号：参数口径 = 核心的 effectivePath（带默认
// 扩展名的实际目标；layout 空/outPath 空时为空串，与旧实现逐字一致）。
PaleoLayoutExportActions::ExportOutcome
PaleoLayoutExportActions::exportLayout( QgsLayout *layout, const QString &outPath, Format format,
                                        double dpi, const PageRange &range )
{
  const ExportOutcome outcome =
      PaleoLayoutExport::exportLayout( layout, outPath, format, dpi, range );
  emit exportFinished( outcome.effectivePath, outcome.ok );
  return outcome;
}

void PaleoLayoutExportActions::runExportUi( Format format )
{
  QgsLayout *layout = m_layoutProvider ? m_layoutProvider() : nullptr;
  if ( !layout )
  {
    if ( m_statusTarget )
      m_statusTarget->showMessage( tr( "没有可导出的版面。" ), 4000 );
    emit exportFinished( QString(), false );
    return;
  }

  QWidget *dialogParent = qobject_cast<QWidget *>( parent() );
  const QString path = QFileDialog::getSaveFileName( dialogParent, tr( "导出版面" ),
                                                     QString(), fileDialogFilter( format ) );
  if ( path.isEmpty() )
    return; // user canceled — no export, no signal

  const int pageCount = layout->pageCollection() ? layout->pageCollection()->pageCount() : 0;
  const int currentPage0 = m_currentPageProvider ? qMax( 0, m_currentPageProvider() ) : 0;

  ExportSettingsDialog dialog( pageCount, currentPage0, dialogParent );
  if ( dialog.exec() != QDialog::Accepted )
    return;

  if ( m_taskSvc && exportLayoutAsync( layout, path, format, dialog.dpi(), dialog.pageRange() ) )
    return;

  // 无任务服务（测试壳）：同步直跑。1200dpi 多页可能跑很久——模态忙等
  // 进度框（导出核心无分页进度，不确定进度条、不可取消，结束即关）。
  QProgressDialog progress( tr( "正在导出版面，请稍候…" ), QString(), 0, 0, dialogParent );
  progress.setWindowTitle( tr( "导出版面" ) );
  progress.setWindowModality( Qt::WindowModal );
  progress.setCancelButton( nullptr );
  progress.setMinimumDuration( 0 );
  progress.setValue( 0 );
  QCoreApplication::processEvents();

  const ExportOutcome outcome = exportLayout( layout, path, format, dialog.dpi(), dialog.pageRange() );
  progress.reset();

  reportExportOutcome( outcome );
}

bool PaleoLayoutExportActions::exportLayoutAsync( QgsLayout *layout, const QString &outPath,
                                                  Format format, double dpi,
                                                  const PageRange &range )
{
  if ( !m_taskSvc || !layout )
    return false;

  // 版面 XML 快照在 GUI 线程取；worker 重建私有 QgsLayout 后导出。
  QDomDocument snapshot( QStringLiteral( "Layout" ) );
  {
    QgsReadWriteContext context;
    snapshot.appendChild( layout->writeXml( snapshot, context ) );
  }
  QgsProject *project = layout->project();
  QPointer<PaleoLayoutExportActions> self = this;
  PaleoTask *task = m_taskSvc->start(
      tr( "导出版面：%1" ).arg( QFileInfo( outPath ).fileName() ),
      [self, snapshot, outPath, format, dpi, range, project]( PaleoTask * ) -> QString {
        QgsPrintLayout rebuilt( project );
        QgsReadWriteContext context;
        if ( !rebuilt.readXml( snapshot.documentElement(), snapshot, context ) )
          return tr( "版面快照重建失败" );
        const PaleoLayoutExport::ExportOutcome outcome =
            PaleoLayoutExport::exportLayout( &rebuilt, outPath, format, dpi, range );
        if ( self )
          self->m_pendingOutcome = outcome;
        return outcome.ok ? QString() : outcome.error;
      } );
  if ( !task )
  {
    reportExportOutcome( ExportOutcome{ false, tr( "任务服务不可用" ), {}, outPath } );
    return false;
  }
  connect( task, &PaleoTask::finished, this, [this] {
    const ExportOutcome outcome = m_pendingOutcome;
    m_pendingOutcome = ExportOutcome();
    emit exportFinished( outcome.effectivePath, outcome.ok );
    reportExportOutcome( outcome );
  } );
  if ( m_statusTarget )
    m_statusTarget->showMessage( tr( "版面导出进行中…" ), 3000 );
  return true;
}

void PaleoLayoutExportActions::reportExportOutcome( const ExportOutcome &outcome )
{
  if ( m_statusTarget )
  {
    if ( outcome.ok )
    {
      m_statusTarget->showMessage( outcome.files.size() > 1
                                     ? tr( "已导出 %1 个文件到 %2" ).arg( outcome.files.size() ).arg( QFileInfo( outcome.files.first() ).absolutePath() )
                                     : tr( "已导出 %1" ).arg( outcome.files.value( 0 ) ), 4000 );
    }
    else
    {
      m_statusTarget->showMessage( outcome.error, 6000 );
    }
  }

  if ( m_openFolderEnabled && outcome.ok && !outcome.files.isEmpty() )
    QDesktopServices::openUrl( QUrl::fromLocalFile( QFileInfo( outcome.files.first() ).absolutePath() ) );
}
