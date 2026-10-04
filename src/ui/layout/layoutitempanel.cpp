// 层：视图
#include "ui/paleotheme.h"
#include "layoutitempanel.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <qgscollapsiblegroupbox.h>
#include <qgsgui.h>
#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemguiregistry.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitemregistry.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutitemwidget.h>
#include <qgslayoutobject.h>
#include <qgslayertree.h>
#include <qgsmaplayer.h>
#include <qgspanelwidgetstack.h>
#include <qgsfillsymbol.h>
#include <qgslinesymbol.h>
#include <qgsproject.h>

// ---------------------------------------------------------------------------
// PaleoCommonItemWidget — fallback QgsLayoutItemBaseWidget for item types with
// no registered per-type GUI metadata (the embedder registry starts empty).
// Deliberately no Q_OBJECT: it adds no signals/slots, and findChild /
// qobject_cast work through the QgsLayoutItemBaseWidget metaobject.
// ---------------------------------------------------------------------------

namespace
{
  class PaleoCommonItemWidget : public QgsLayoutItemBaseWidget
  {
    public:
      explicit PaleoCommonItemWidget( QgsLayoutItem *item, QWidget *parent = nullptr )
        : QgsLayoutItemBaseWidget( parent, item )
      {
        setPanelTitle( QObject::tr( "项属性" ) );
        auto *lay = new QVBoxLayout( this );
        lay->setContentsMargins( 0, 0, 0, 0 );
        lay->setSpacing( 0 );
        // Native common-properties control surface: position/size/rotation,
        // background, frame, item id (public in qgslayoutitemwidget.h).
        lay->addWidget( new QgsLayoutItemPropertiesWidget( this, item ) );
      }
  };

  void ensureDefaultItemGuiMetadata()
  {
    QgsLayoutItemGuiRegistry *registry = QgsGui::layoutItemGuiRegistry();

    // Known item types a designer session realistically hosts. Page/group are
    // skipped on purpose. Only fill gaps: never clobber richer metadata that
    // app-level integration may have registered for a type.
    const int knownTypes[] = {
      QgsLayoutItemRegistry::LayoutMap,
      QgsLayoutItemRegistry::LayoutPicture,
      QgsLayoutItemRegistry::LayoutLabel,
      QgsLayoutItemRegistry::LayoutLegend,
      QgsLayoutItemRegistry::LayoutShape,
      QgsLayoutItemRegistry::LayoutPolygon,
      QgsLayoutItemRegistry::LayoutPolyline,
      QgsLayoutItemRegistry::LayoutScaleBar,
      QgsLayoutItemRegistry::LayoutAttributeTable,
      QgsLayoutItemRegistry::LayoutManualTable,
      QgsLayoutItemRegistry::LayoutMarker,
      QgsLayoutItemRegistry::LayoutElevationProfile,
      QgsLayoutItemRegistry::LayoutChart,
    };

    for ( int type : knownTypes )
    {
      if ( registry->metadataIdForItemType( type ) != -1 )
        continue;
      registry->addLayoutItemGuiMetadata( new QgsLayoutItemGuiMetadata(
        type, QObject::tr( "项属性" ), QIcon(),
        []( QgsLayoutItem * item ) -> QgsLayoutItemBaseWidget *
        { return new PaleoCommonItemWidget( item ); } ) );
    }
  }
} // namespace

// ---------------------------------------------------------------------------
// PaleoLayoutItemPanel
// ---------------------------------------------------------------------------

PaleoLayoutItemPanel::PaleoLayoutItemPanel( QWidget *parent )
  : QWidget( parent )
{
  // Registered here (not in main): integrators' QSignalSpy/queued connections
  // on itemChanged(QgsLayoutItem*) must work without extra setup.
  qRegisterMetaType<QgsLayoutItem *>( );

  setObjectName( QStringLiteral( "PaleoLayoutItemPanel" ) );
  setWindowTitle( tr( "项属性" ) );

  auto *root = new QVBoxLayout( this );
  root->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm); // DESIGN spacing.sm panel padding
  root->setSpacing(PaleoTheme::tokens().spacingSm);

  // --- native host / placeholder pages -------------------------------------

  m_pages = new QStackedWidget( this );

  m_placeholder = new QLabel( tr( "从版面中选择一个项，以查看并编辑其属性。" ), m_pages );
  m_placeholder->setObjectName( QStringLiteral( "placeholderLabel" ) );
  m_placeholder->setWordWrap( true );
  m_placeholder->setAlignment( Qt::AlignTop | Qt::AlignLeft );
  m_pages->addWidget( m_placeholder );

  m_stack = new QgsPanelWidgetStack( m_pages ); // native QGIS panel stack
  m_pages->addWidget( m_stack );

  root->addWidget( m_pages, 1 );
  m_pages->setCurrentWidget( m_placeholder );

  // --- Paleo business override区 -------------------------------------------

  auto *business = new QgsCollapsibleGroupBox( tr( "层位联动" ), this );
  business->setObjectName( QStringLiteral( "horizonLinkGroup" ) );
  auto *grid = new QGridLayout( business );
  grid->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  grid->setHorizontalSpacing( PaleoTheme::tokens().spacingSm );
  grid->setVerticalSpacing( PaleoTheme::tokens().spacingXs );

  auto *horizonLabel = new QLabel( tr( "当前活动层位" ), business );
  m_horizonValue = new QLabel( business );
  m_horizonValue->setObjectName( QStringLiteral( "horizonTitleLabel" ) );

  m_applyHorizonButton = new QPushButton( tr( "注入标题" ), business );
  m_applyHorizonButton->setObjectName( QStringLiteral( "applyHorizonButton" ) );
  connect( m_applyHorizonButton, &QPushButton::clicked, this, [this] { applyHorizonTitle(); } );

  grid->addWidget( horizonLabel, 0, 0 );
  grid->addWidget( m_horizonValue, 0, 1 );
  grid->addWidget( m_applyHorizonButton, 0, 2 );
  grid->setColumnStretch( 1, 1 );

  auto *presetCaption = new QLabel( tr( "比例尺预设" ), business );
  auto *presetRow = new QHBoxLayout();
  presetRow->setSpacing(PaleoTheme::tokens().spacingXs); // DESIGN spacing.xs
  const struct
  {
      const char *objectName;
      const char *text;
      int preset;
  } presets[] = {
    { "presetSingleBoxButton", "单厢简洁", 0 },
    { "presetDoubleBoxButton", "黑白双厢", 1 },
    { "presetLineTicksButton", "线段刻度", 2 },
  };
  for ( const auto &p : presets )
  {
    auto *btn = new QPushButton( tr( p.text ), business );
    btn->setObjectName( QString::fromLatin1( p.objectName ) );
    const int preset = p.preset;
    connect( btn, &QPushButton::clicked, this, [this, preset] { applyScalebarPreset( preset ); } );
    presetRow->addWidget( btn );
    m_presetButtons[preset] = btn;
  }
  presetRow->addStretch( 1 );

  grid->addWidget( presetCaption, 1, 0, 1, 3 );
  grid->addLayout( presetRow, 2, 0, 1, 3 );

  // 地图内容绑定（方向 25 M3）：快照（锁定当前层集）/ 实时（跟随图层树主题）。
  auto *bindingCaption = new QLabel( tr( "地图内容" ), business );
  auto *bindingRow = new QHBoxLayout();
  bindingRow->setSpacing(PaleoTheme::tokens().spacingXs); // DESIGN spacing.xs
  m_mapSnapshotButton = new QPushButton( tr( "锁定快照" ), business );
  m_mapSnapshotButton->setObjectName( QStringLiteral( "mapSnapshotButton" ) );
  connect( m_mapSnapshotButton, &QPushButton::clicked, this,
           [this] { applyMapContentBinding( true ); } );
  m_mapLiveButton = new QPushButton( tr( "实时跟随" ), business );
  m_mapLiveButton->setObjectName( QStringLiteral( "mapLiveButton" ) );
  connect( m_mapLiveButton, &QPushButton::clicked, this,
           [this] { applyMapContentBinding( false ); } );
  bindingRow->addWidget( m_mapSnapshotButton );
  bindingRow->addWidget( m_mapLiveButton );
  bindingRow->addStretch( 1 );

  grid->addWidget( bindingCaption, 3, 0, 1, 3 );
  grid->addLayout( bindingRow, 4, 0, 1, 3 );

  root->addWidget( business );

  updateBusinessControls();
}

void PaleoLayoutItemPanel::setItem( QgsLayoutItem *item )
{
  if ( m_item.data() == item )
    return; // same item (or both null): never rebuild

  if ( m_item )
    disconnect( m_item.data(), nullptr, this, nullptr );

  m_item = item;

  if ( m_item )
  {
    // QgsLayoutItem is-a QgsLayoutObject is-a QObject: destroyed() is the
    // dangling guard (m_item is a QPointer and is already null in the slot).
    connect( m_item.data(), &QObject::destroyed, this, [this]
    {
      m_item = nullptr;
      clearHostedWidget();
      updateBusinessControls();
      emit itemChanged( nullptr );
    } );

    // Coarse-grained change channel: native widgets and panel actions mutate
    // the item through its setters, which emit QgsLayoutObject::changed().
    connect( m_item.data(), &QgsLayoutObject::changed, this, [this]
    {
      emit itemChanged( m_item.data() );
    } );
  }

  hostItemWidget();
  updateBusinessControls();
}

QgsLayoutItem *PaleoLayoutItemPanel::item() const
{
  return m_item.data();
}

void PaleoLayoutItemPanel::setActiveHorizonTitle( const QString &title )
{
  if ( m_horizonTitle == title )
    return;
  m_horizonTitle = title;
  updateBusinessControls();
}

bool PaleoLayoutItemPanel::applyHorizonTitle()
{
  auto *label = qobject_cast<QgsLayoutItemLabel *>( m_item.data() );
  if ( !label || m_horizonTitle.isEmpty() )
    return false;

  label->setText( m_horizonTitle ); // full replacement; emits changed() → itemChanged
  return true;
}

void PaleoLayoutItemPanel::applyScalebarPreset( int preset )
{
  auto *scalebar = qobject_cast<QgsLayoutItemScaleBar *>( m_item.data() );
  if ( !scalebar )
    return; // no-op; the preset buttons carry the §35 reason tooltip

  switch ( preset )
  {
    case 0: // 单厢简洁
      scalebar->setStyle( QStringLiteral( "Single Box" ) );
      scalebar->setUnits( Qgis::DistanceUnit::Kilometers );
      scalebar->setUnitLabel( QStringLiteral( "km" ) );
      scalebar->setNumberOfSegments( 4 );
      scalebar->setNumberOfSegmentsLeft( 2 );
      break;

    case 1: // 黑白双厢
    {
      scalebar->setStyle( QStringLiteral( "Double Box" ) );
      scalebar->setUnits( Qgis::DistanceUnit::Kilometers );
      scalebar->setUnitLabel( QStringLiteral( "km" ) );
      scalebar->setNumberOfSegments( 6 );
      scalebar->setNumberOfSegmentsLeft( 0 );
      scalebar->setFillSymbol( QgsFillSymbol::createSimple( {
        { QStringLiteral( "color" ), QStringLiteral( "255,255,255,255" ) },
        { QStringLiteral( "outline_style" ), QStringLiteral( "no" ) },
      } ).release() );
      scalebar->setAlternateFillSymbol( QgsFillSymbol::createSimple( {
        { QStringLiteral( "color" ), QStringLiteral( "0,0,0,255" ) },
        { QStringLiteral( "outline_style" ), QStringLiteral( "no" ) },
      } ).release() );
      scalebar->setLineSymbol( QgsLineSymbol::createSimple( {
        { QStringLiteral( "line_color" ), QStringLiteral( "0,0,0,255" ) },
        { QStringLiteral( "line_width" ), QStringLiteral( "0.2" ) },
      } ).release() );
      break;
    }

    case 2: // 线段刻度
      scalebar->setStyle( QStringLiteral( "Line Ticks Down" ) );
      scalebar->setUnits( Qgis::DistanceUnit::Meters );
      scalebar->setUnitLabel( QStringLiteral( "m" ) );
      scalebar->setNumberOfSegments( 5 );
      scalebar->setNumberOfSegmentsLeft( 0 );
      break;

    default:
      return; // unknown preset id: no-op
  }

  scalebar->update();
  // The setters emit changed() (forwarded as itemChanged); the explicit emit
  // guarantees notification even for setters that only invalidate the cache.
  emit itemChanged( m_item.data() );
}

bool PaleoLayoutItemPanel::applyMapContentBinding( bool snapshot )
{
  auto *map = qobject_cast<QgsLayoutItemMap *>( m_item.data() );
  if ( !map )
    return false;

  if ( !snapshot )
  {
    map->setFollowVisibilityPreset( true ); // 主题名沿用已钉定的（含空名=当前层集）
    emit itemChanged( m_item.data() );
    return true;
  }

  // 快照：当前有效层集钉为显式层集。存储层集为空（跟随态）时从工程图层树
  // 取勾选层——layout() 给不出工程时回退 QgsProject 单例（隔离测试壳同款）。
  QList<QgsMapLayer *> pin = map->layers();
  if ( pin.isEmpty() )
  {
    QgsProject *project = map->layout() && map->layout()->project()
                              ? map->layout()->project()
                              : QgsProject::instance();
    if ( project && project->layerTreeRoot() )
      pin = project->layerTreeRoot()->checkedLayers();
  }
  map->setFollowVisibilityPreset( false );
  if ( !pin.isEmpty() )
    map->setLayers( pin );
  emit itemChanged( m_item.data() );
  return true;
}

void PaleoLayoutItemPanel::hostItemWidget()
{
  clearHostedWidget();

  if ( !m_item )
  {
    m_pages->setCurrentWidget( m_placeholder );
    return;
  }

  ensureDefaultItemGuiMetadata();

  QgsLayoutItemBaseWidget *widget = QgsGui::layoutItemGuiRegistry()->createItemWidget( m_item.data() );
  if ( !widget )
    widget = new PaleoCommonItemWidget( m_item.data() ); // type without metadata
  widget->setDockMode( true );
  m_stack->setMainPanel( widget ); // stack takes ownership
  m_pages->setCurrentWidget( m_stack );
}

void PaleoLayoutItemPanel::clearHostedWidget()
{
  // Synchronous delete (not deleteLater): callers inspect the widget tree
  // right after setItem()/clear, and a deferred delete would leave the old
  // QgsLayoutItemBaseWidget discoverable by findChild().
  if ( QgsPanelWidget *main = m_stack->takeMainPanel() )
    delete main;
  m_pages->setCurrentWidget( m_placeholder );
}

void PaleoLayoutItemPanel::updateBusinessControls()
{
  auto *label = qobject_cast<QgsLayoutItemLabel *>( m_item.data() );
  const bool canInject = label && !m_horizonTitle.isEmpty();
  m_applyHorizonButton->setEnabled( canInject );
  m_applyHorizonButton->setToolTip( canInject
                                      ? tr( "将标签文本替换为当前活动层位标题" )
                                      : ( label ? tr( "未设置活动层位" )
                                                : tr( "仅当选中的版面项是标签时可用" ) ) );

  auto *scalebar = qobject_cast<QgsLayoutItemScaleBar *>( m_item.data() );
  for ( QPushButton *btn : m_presetButtons )
  {
    if ( !btn )
      continue;
    btn->setEnabled( scalebar != nullptr );
    btn->setToolTip( scalebar ? tr( "套用该比例尺样式预设" )
                              : tr( "仅当选中的版面项是比例尺时可用" ) );
  }

  auto *mapItem = qobject_cast<QgsLayoutItemMap *>( m_item.data() );
  if ( m_mapSnapshotButton && m_mapLiveButton )
  {
    const bool isMap = mapItem != nullptr;
    m_mapSnapshotButton->setEnabled( isMap );
    m_mapLiveButton->setEnabled( isMap );
    const QString reason = isMap ? QString()
                                 : tr( "仅当选中的版面项是地图时可用" );
    m_mapSnapshotButton->setToolTip( isMap ? tr( "停止跟随主题，把当前层集钉为快照" ) : reason );
    m_mapLiveButton->setToolTip( isMap ? tr( "恢复跟随图层树主题（实时）" ) : reason );
  }

  m_horizonValue->setText( m_horizonTitle.isEmpty() ? tr( "未设置" ) : m_horizonTitle );
}
