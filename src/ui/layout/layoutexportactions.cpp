// 层：视图
#include "layoutexportactions.h"

#include <QAction>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressDialog>
#include <QRadioButton>
#include <QSet>
#include <QSpinBox>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include <QDomDocument>
#include <qgslayertree.h>
#include <qgslayout.h>
#include <qgslayoutpagecollection.h>
#include <qgsmaplayer.h>
#include <qgsmaplayerfactory.h>
#include <qgsmapthemecollection.h>
#include <qgspathresolver.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsreadwritecontext.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>

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

        // 方向 25 M4：图件分辨率档位（150 屏阅 / 300 印刷 / 600 高精度），
        // 一键回填自旋框——自旋框仍是唯一真值（档位外可手输）。
        auto *presetRow = new QHBoxLayout;
        presetRow->setSpacing( 4 ); // DESIGN spacing.xs
        const struct
        {
            int dpi;
            const char *objectName;
        } dpiPresets[] = { { 150, "dpiPreset150Button" }, { 300, "dpiPreset300Button" },
                           { 600, "dpiPreset600Button" } };
        for ( const auto &preset : dpiPresets )
        {
          auto *btn = new QPushButton( tr( "%1 dpi" ).arg( preset.dpi ), this );
          btn->setObjectName( QString::fromLatin1( preset.objectName ) );
          btn->setToolTip( tr( "回填 %1 dpi 预设" ).arg( preset.dpi ) );
          const int dpi = preset.dpi;
          connect( btn, &QPushButton::clicked, this,
                   [this, dpi] { m_dpiSpin->setValue( dpi ); } );
          presetRow->addWidget( btn );
        }
        presetRow->addStretch();

        auto *form = new QFormLayout;
        form->addRow( tr( "分辨率" ), m_dpiSpin );
        form->addRow( QString(), presetRow );

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

  if ( m_taskSvc )
  {
    QString fallbackReason;
    if ( exportLayoutAsync( layout, path, format, dialog.dpi(), dialog.pageRange(), &fallbackReason ) )
      return;
    if ( m_statusTarget && !fallbackReason.isEmpty() )
      m_statusTarget->showMessage( tr( "%1——改为前台导出" ).arg( fallbackReason ), 4000 );
  }

  // 同步路径（无任务服务的测试壳，或工程无法隔离到 worker）。#85：不再
  // processEvents 泵事件「让进度框上屏」（点击回调里重入），只挂等待光标。
  QGuiApplication::setOverrideCursor( Qt::WaitCursor );
  const ExportOutcome outcome = exportLayout( layout, path, format, dialog.dpi(), dialog.pageRange() );
  QGuiApplication::restoreOverrideCursor();

  reportExportOutcome( outcome );
}

void PaleoLayoutExportActions::setInFlight( bool inFlight )
{
  m_inFlight = inFlight;
  for ( QAction *action : { m_pngAction, m_pdfAction, m_svgAction } )
  {
    if ( action )
      action->setEnabled( !inFlight );
  }
}

namespace
{
  const QString kSnapRoot = QStringLiteral( "qgis" );
  const QString kSnapLayers = QStringLiteral( "projectlayers" );
  const QString kSnapTree = QStringLiteral( "layertree" );

  QgsReadWriteContext absolutePathContext()
  {
    QgsReadWriteContext context;
    context.setPathResolver( QgsPathResolver() ); // 空 base：路径原样（绝对）读写
    return context;
  }
} // namespace

bool PaleoLayoutExportActions::captureProjectSnapshot( const QgsProject *project,
                                                       ProjectSnapshot &out, QString *reason )
{
  auto fail = [reason]( const QString &why ) {
    if ( reason )
      *reason = why;
    return false;
  };
  out = ProjectSnapshot();
  QDomDocument doc( kSnapRoot );
  QDomElement root = doc.createElement( kSnapRoot );
  doc.appendChild( root );
  if ( !project )
  {
    out.xml = doc;
    return true; // 无工程：版面只含非地图项，空私有工程即可
  }

  const QgsReadWriteContext context = absolutePathContext();
  QDomElement layersEl = doc.createElement( kSnapLayers );
  root.appendChild( layersEl );
  const QMap<QString, QgsMapLayer *> layers = project->mapLayers();
  for ( auto it = layers.cbegin(); it != layers.cend(); ++it )
  {
    QgsMapLayer *layer = it.value();
    if ( !layer )
      continue;
    if ( layer->type() == Qgis::LayerType::Plugin )
      return fail( tr( "图层「%1」是插件图层，无法在后台重建" ).arg( layer->name() ) );
    auto *vector = qobject_cast<QgsVectorLayer *>( layer );
    if ( vector && vector->isEditable() && vector->isModified() )
      return fail( tr( "图层「%1」有未提交的编辑" ).arg( layer->name() ) );
    QDomElement layerEl = doc.createElement( QStringLiteral( "maplayer" ) );
    if ( !layer->writeLayerXml( layerEl, doc, context ) )
      return fail( tr( "图层「%1」序列化失败" ).arg( layer->name() ) );
    layersEl.appendChild( layerEl );
    // memory 图层的 XML 只有字段定义没有要素：要素在 GUI 线程拷出（值类型，
    // 隐式共享只读），worker 重建后灌回私有图层。
    if ( vector && vector->providerType() == QLatin1String( "memory" ) )
    {
      QgsFeatureList features;
      QgsFeature feature;
      QgsFeatureIterator fit = vector->getFeatures();
      while ( fit.nextFeature( feature ) )
        features.append( feature );
      out.memoryFeatures.insert( layer->id(), features );
    }
  }

  QDomElement treeEl = doc.createElement( kSnapTree );
  root.appendChild( treeEl );
  if ( QgsLayerTree *tree = const_cast<QgsProject *>( project )->layerTreeRoot() )
    tree->writeXml( treeEl, context );
  if ( QgsMapThemeCollection *themes = const_cast<QgsProject *>( project )->mapThemeCollection() )
    themes->writeXml( doc ); // 写进 <qgis><visibility-presets>

  out.xml = doc;
  out.crs = project->crs();
  out.transformContext = project->transformContext();
  out.ellipsoid = project->ellipsoid();
  out.title = project->title();
  out.homePath = project->homePath();
  out.customVariables = project->customVariables();
  return true;
}

std::unique_ptr<QgsProject> PaleoLayoutExportActions::rebuildProject( const ProjectSnapshot &snapshot,
                                                                      QString *error )
{
  auto project = std::make_unique<QgsProject>();
  project->setCrs( snapshot.crs );
  project->setTransformContext( snapshot.transformContext );
  if ( !snapshot.ellipsoid.isEmpty() )
    project->setEllipsoid( snapshot.ellipsoid );
  project->setTitle( snapshot.title );
  if ( !snapshot.homePath.isEmpty() )
    project->setPresetHomePath( snapshot.homePath );
  project->setCustomVariables( snapshot.customVariables );

  QgsReadWriteContext context = absolutePathContext();
  context.setTransformContext( snapshot.transformContext );
  const QDomElement root = snapshot.xml.documentElement();

  QList<QgsMapLayer *> layers;
  const QDomElement layersEl = root.firstChildElement( kSnapLayers );
  for ( QDomElement layerEl = layersEl.firstChildElement( QStringLiteral( "maplayer" ) ); !layerEl.isNull();
        layerEl = layerEl.nextSiblingElement( QStringLiteral( "maplayer" ) ) )
  {
    bool typeOk = false;
    const Qgis::LayerType type = QgsMapLayerFactory::typeFromString( layerEl.attribute( QStringLiteral( "type" ) ), typeOk );
    if ( !typeOk )
      continue;
    QgsMapLayerFactory::LayerOptions options( snapshot.transformContext );
    options.loadDefaultStyle = false;
    const QString provider = layerEl.firstChildElement( QStringLiteral( "provider" ) ).text();
    std::unique_ptr<QgsMapLayer> layer( QgsMapLayerFactory::createLayer( QString(), QString(), type, options, provider ) );
    if ( !layer )
      continue;
    // readLayerXml 失败（数据源离线等）仍保留该图层：与活工程里「无效图层」
    // 同口径，地图项照常跳过它，而不是整次导出失败。
    layer->readLayerXml( layerEl, context );
    if ( auto *vector = qobject_cast<QgsVectorLayer *>( layer.get() ) )
    {
      const auto feats = snapshot.memoryFeatures.constFind( vector->id() );
      if ( feats != snapshot.memoryFeatures.cend() && vector->dataProvider() )
      {
        QgsFeatureList copy = feats.value();
        vector->dataProvider()->addFeatures( copy );
        vector->updateExtents();
      }
    }
    layers.append( layer.release() );
  }
  if ( !layers.isEmpty() && project->addMapLayers( layers, false ).size() != layers.size() )
  {
    if ( error )
      *error = tr( "私有工程装载图层失败" );
    return nullptr;
  }
  for ( QgsMapLayer *layer : std::as_const( layers ) )
    layer->resolveReferences( project.get() );

  const QDomElement treeGroup = root.firstChildElement( kSnapTree ).firstChildElement( QStringLiteral( "layer-tree-group" ) );
  if ( !treeGroup.isNull() )
  {
    QgsLayerTree *tree = project->layerTreeRoot();
    tree->readChildrenFromXml( treeGroup, context );
    tree->resolveReferences( project.get() );
    const QDomElement order = treeGroup.firstChildElement( QStringLiteral( "custom-order" ) );
    if ( !order.isNull() )
    {
      QStringList ids;
      for ( QDomElement item = order.firstChildElement( QStringLiteral( "item" ) ); !item.isNull();
            item = item.nextSiblingElement( QStringLiteral( "item" ) ) )
        ids << item.text();
      tree->setCustomLayerOrder( ids );
      tree->setHasCustomLayerOrder( order.attribute( QStringLiteral( "enabled" ) ).toInt() != 0 );
    }
  }
  project->mapThemeCollection()->readXml( snapshot.xml );
  return project;
}

bool PaleoLayoutExportActions::exportLayoutAsync( QgsLayout *layout, const QString &outPath,
                                                  Format format, double dpi,
                                                  const PageRange &range, QString *fallbackReason )
{
  if ( !m_taskSvc || !layout )
    return false;

  // 同一实例一次只跑一个任务池导出：动作已禁用，这里兜住编程调用。
  if ( m_inFlight )
  {
    const ExportOutcome busy{ false, tr( "已有版面导出正在进行，请等待其完成" ), {}, outPath };
    emit exportFinished( busy.effectivePath, false );
    reportExportOutcome( busy );
    return true;
  }

  // 每个任务一份作业状态：work（worker）写 outcome，finished 回包（GUI）读。
  // 回包经任务服务排队到 GUI 线程且在 work 返回之后，读写天然有先后，
  // 不需要跨线程判活/加锁。
  struct Job
  {
    QDomDocument layoutXml;
    ProjectSnapshot project;
    ExportOutcome outcome;
  };
  auto job = std::make_shared<Job>();
  if ( !captureProjectSnapshot( layout->project(), job->project, fallbackReason ) )
    return false;
  job->layoutXml = QDomDocument( QStringLiteral( "Layout" ) );
  {
    QgsReadWriteContext context;
    job->layoutXml.appendChild( layout->writeXml( job->layoutXml, context ) );
  }
  job->outcome = ExportOutcome{ false, tr( "版面导出已取消" ), {}, outPath };

  PaleoTask *task = m_taskSvc->start(
      tr( "导出版面：%1" ).arg( QFileInfo( outPath ).fileName() ),
      [job, outPath, format, dpi, range]( PaleoTask *t ) -> QString {
        if ( t && t->cancelRequested() )
          return QString();
        QString error;
        std::unique_ptr<QgsProject> project = rebuildProject( job->project, &error );
        if ( !project )
        {
          job->outcome = ExportOutcome{ false, error, {}, outPath };
          return error;
        }
        {
          QgsPrintLayout rebuilt( project.get() );
          QgsReadWriteContext context;
          if ( !rebuilt.readXml( job->layoutXml.documentElement(), job->layoutXml, context ) )
          {
            job->outcome = ExportOutcome{ false, tr( "版面快照重建失败" ), {}, outPath };
            return job->outcome.error;
          }
          if ( t && t->cancelRequested() )
            return QString();
          job->outcome = PaleoLayoutExport::exportLayout( &rebuilt, outPath, format, dpi, range );
        } // 版面先于其工程析构
        return job->outcome.ok ? QString() : job->outcome.error;
      } );
  if ( !task )
  {
    reportExportOutcome( ExportOutcome{ false, tr( "任务服务不可用" ), {}, outPath } );
    return false;
  }
  setInFlight( true );
  connect( task, &PaleoTask::finished, this, [this, job] {
    setInFlight( false );
    const ExportOutcome outcome = job->outcome;
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
