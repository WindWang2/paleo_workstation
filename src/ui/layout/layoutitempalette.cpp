// 层：视图
#include "ui/paleoicons.h"
#include "layoutitempalette.h"
#include "../paleotheme.h"

#include <QCoreApplication>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include <memory>

#include <qgsapplication.h>
#include <qgsgui.h>
#include <qgslayout.h>
#include <qgslayoutframe.h>
#include <qgslayoutguiutils.h>
#include <qgslayoutitemguiregistry.h>
#include <qgslayoutitemnodeitem.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitemregistry.h>
#include <qgslayoutitemtexttable.h>
#include <qgslayoutmultiframe.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutundostack.h>
#include <qgslayoutview.h>
#include <qgslayoutviewtooladditem.h>

namespace
{
  // Nominal A4 portrait in mm — fallback page rect for layouts that have no
  // pages yet (a fresh QgsPrintLayout starts with zero pages).
  const QRectF kNominalPageRect( 0.0, 0.0, 210.0, 297.0 );

  // QGIS's default GUI metadata carries English visible names straight into
  // button text / undo macros. 2026-09-29 决策（源语言=中文）：把已知的注册名
  // 映射成中文源串；未知名原样返回（兜底，不脱译）。
  QString localizedItemName( const QString &visibleName )
  {
    static const QHash<QString, const char *> kNames = {
      { QStringLiteral( "Map" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "地图" ) },
      { QStringLiteral( "Legend" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "图例" ) },
      { QStringLiteral( "Scale Bar" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "比例尺" ) },
      { QStringLiteral( "Label" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "标注" ) },
      { QStringLiteral( "Picture" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "图片" ) },
      { QStringLiteral( "North Arrow" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "指北针" ) },
      { QStringLiteral( "Arrow" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "箭头" ) },
      { QStringLiteral( "Rectangle" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "矩形" ) },
      { QStringLiteral( "Ellipse" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "椭圆" ) },
      { QStringLiteral( "Triangle" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "三角形" ) },
      { QStringLiteral( "Marker" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "点标记" ) },
      { QStringLiteral( "Polygon" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "多边形" ) },
      { QStringLiteral( "Polyline" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "折线" ) },
      { QStringLiteral( "HTML" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "HTML" ) },
      { QStringLiteral( "Attribute Table" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "属性表" ) },
      { QStringLiteral( "Fixed Table" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "固定表格" ) },
      { QStringLiteral( "Text Table" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "文本表格" ) },
      { QStringLiteral( "Elevation Profile" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "高程剖面" ) },
      { QStringLiteral( "Chart" ), QT_TRANSLATE_NOOP( "PaleoLayoutItemPalette", "图表" ) },
    };
    const auto it = kNames.constFind( visibleName );
    return it != kNames.constEnd()
               ? QCoreApplication::translate( "PaleoLayoutItemPalette", it.value() )
               : visibleName;
  }

  // Default sizes (mm) for programmatic placement, matching the feel of the
  // QGIS designer's defaults for drag-created items.
  QSizeF defaultSizeFor( const QgsLayoutItemAbstractGuiMetadata *metadata )
  {
    switch ( metadata->type() )
    {
      case QgsLayoutItemRegistry::LayoutMap:
        return QSizeF( 150.0, 150.0 );
      case QgsLayoutItemRegistry::LayoutLegend:
        return QSizeF( 80.0, 60.0 );
      case QgsLayoutItemRegistry::LayoutScaleBar:
        return QSizeF( 60.0, 10.0 );
      case QgsLayoutItemRegistry::LayoutLabel:
        return QSizeF( 80.0, 15.0 );
      case QgsLayoutItemRegistry::LayoutPicture:
        // the north-arrow variant wants a small, arrow-sized frame
        return metadata->visibleName() == QLatin1String( "North Arrow" ) ? QSizeF( 16.0, 16.0 )
                                                                        : QSizeF( 40.0, 40.0 );
      case QgsLayoutItemRegistry::LayoutShape:
      case QgsLayoutItemRegistry::LayoutMarker:
        return metadata->type() == QgsLayoutItemRegistry::LayoutMarker ? QSizeF( 8.0, 8.0 )
                                                                       : QSizeF( 40.0, 30.0 );
      case QgsLayoutItemRegistry::LayoutPolygon:
      case QgsLayoutItemRegistry::LayoutPolyline:
        return QSizeF( 60.0, 40.0 );
      case QgsLayoutItemRegistry::LayoutHtml:
      case QgsLayoutItemRegistry::LayoutAttributeTable:
      case QgsLayoutItemRegistry::LayoutManualTable:
      case QgsLayoutItemRegistry::LayoutTextTable:
        return QSizeF( 120.0, 60.0 );
      case QgsLayoutItemRegistry::LayoutElevationProfile:
        return QSizeF( 150.0, 80.0 );
      case QgsLayoutItemRegistry::LayoutChart:
        return QSizeF( 100.0, 80.0 );
      default:
        return QSizeF( 60.0, 40.0 );
    }
  }

  QRectF firstPageRect( QgsLayout *layout )
  {
    if ( !layout )
      return kNominalPageRect;
    QgsLayoutPageCollection *pages = layout->pageCollection();
    if ( pages && pages->pageCount() > 0 )
    {
      QgsLayoutItemPage *page = pages->page( 0 );
      if ( page )
        return QRectF( page->pos(), layout->convertToLayoutUnits( page->pageSize() ) );
    }
    return kNominalPageRect;
  }
} // namespace

PaleoLayoutItemPalette::PaleoLayoutItemPalette( QWidget *parent )
  : QWidget( parent )
{
  setObjectName( QStringLiteral( "PaleoLayoutItemPalette" ) );
  ensureDefaultItemMetadataRegistered();

  auto *vbox = new QVBoxLayout( this );
  vbox->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm); // DESIGN.md spacing.sm
  vbox->setSpacing(PaleoTheme::tokens().spacingMd);                 // DESIGN.md spacing.md

  // --- page properties entry (own signal: LayoutPage has no GUI metadata)
  auto *pageButton = new QToolButton( this );
  pageButton->setObjectName( QStringLiteral( "btnPageProperties" ) );
  pageButton->setText( tr( "页面属性" ) );
  pageButton->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "mActionNewPage.svg" ) ) );
  pageButton->setToolButtonStyle( Qt::ToolButtonTextUnderIcon );
  pageButton->setToolTip( tr( "打开当前版面的页面设置" ) );
  connect( pageButton, &QToolButton::clicked, this, &PaleoLayoutItemPalette::pagePropertiesRequested );
  vbox->addWidget( pageButton );

  auto *separator = new QFrame( this );
  separator->setFrameShape( QFrame::HLine );
  separator->setFrameShadow( QFrame::Plain );
  vbox->addWidget( separator );

  // --- element groups -------------------------------------------------------
  const QList<ButtonSpec> mainSpecs = {
    { "btnAddMap", QgsLayoutItemRegistry::LayoutMap, nullptr, "Map" },
    { "btnAddLegend", QgsLayoutItemRegistry::LayoutLegend, nullptr, "Legend" },
    { "btnAddScaleBar", QgsLayoutItemRegistry::LayoutScaleBar, nullptr, "Scale Bar" },
    { "btnAddLabel", QgsLayoutItemRegistry::LayoutLabel, nullptr, "Label" },
    { "btnAddPicture", QgsLayoutItemRegistry::LayoutPicture, nullptr, "Picture" },
    { "btnAddNorthArrow", QgsLayoutItemRegistry::LayoutPicture, "North Arrow", "North Arrow" },
    { "btnAddRectangle", QgsLayoutItemRegistry::LayoutShape, "Rectangle", "Rectangle" },
    { "btnAddEllipse", QgsLayoutItemRegistry::LayoutShape, "Ellipse", "Ellipse" },
    { "btnAddPolygon", QgsLayoutItemRegistry::LayoutPolygon, nullptr, "Polygon" },
    { "btnAddPolyline", QgsLayoutItemRegistry::LayoutPolyline, "Polyline", "Polyline" },
  };
  const QList<ButtonSpec> secondarySpecs = {
    { "btnAddAttributeTable", QgsLayoutItemRegistry::LayoutAttributeTable, nullptr, "Attribute Table" },
    { "btnAddManualTable", QgsLayoutItemRegistry::LayoutManualTable, nullptr, "Fixed Table" },
    { "btnAddTextTable", QgsLayoutItemRegistry::LayoutTextTable, nullptr, "Text Table" },
    { "btnAddElevationProfile", QgsLayoutItemRegistry::LayoutElevationProfile, nullptr, "Elevation Profile" },
    { "btnAddChart", QgsLayoutItemRegistry::LayoutChart, nullptr, "Chart" },
    { "btnAddMarker", QgsLayoutItemRegistry::LayoutMarker, nullptr, "Marker" },
  };

  vbox->addWidget( buildGroup( QStringLiteral( "mainElements" ), tr( "主要元素" ), mainSpecs ) );
  vbox->addWidget( buildGroup( QStringLiteral( "secondaryElements" ), tr( "次要元素" ), secondarySpecs ) );
  vbox->addStretch( 1 );
}

PaleoLayoutItemPalette::~PaleoLayoutItemPalette()
{
  if ( m_attachConnection )
    disconnect( m_attachConnection );
}

bool PaleoLayoutItemPalette::ensureDefaultItemMetadataRegistered()
{
  QgsLayoutItemGuiRegistry *registry = QgsGui::layoutItemGuiRegistry();
  if ( !registry )
    return false;

  // QGIS's default 18 entries (Map..Chart). Guard on a representative type,
  // not on emptiness: the registry may already hold custom entries.
  if ( registry->metadataIdForItemType( QgsLayoutItemRegistry::LayoutMap ) < 0 )
    QgsLayoutGuiUtils::registerGuiForKnownItemTypes( nullptr ); // null map canvas is supported

  // Text tables have no default GUI metadata in QGIS 4.2 — register one that
  // mirrors upstream's attribute-table metadata (multiframe factory path).
  if ( registry->metadataIdForItemType( QgsLayoutItemRegistry::LayoutTextTable ) < 0 )
  {
    registry->addLayoutItemGuiMetadata( new QgsLayoutItemGuiMetadata(
      QgsLayoutItemRegistry::LayoutTextTable,
      QCoreApplication::translate( "PaleoLayoutItemPalette", "Text Table" ),
      PaleoIcons::qgisTheme( QStringLiteral( "mActionAddTable.svg" ) ),
      nullptr, // no per-item config widget: generic frame handling applies
      nullptr, // default rectangular rubber band
      QString(), false, QgsLayoutItemAbstractGuiMetadata::Flags(),
      []( QgsLayout *layout ) -> QgsLayoutItem * {
        if ( !layout )
          return nullptr;
        auto *multiFrame = new QgsLayoutItemTextTable( layout );
        layout->addMultiFrame( multiFrame );
        auto *frame = new QgsLayoutFrame( layout, multiFrame );
        multiFrame->addFrame( frame ); // adds the frame to the layout
        return frame;
      } ) );
  }
  return true;
}

void PaleoLayoutItemPalette::attach( QgsLayoutView *view )
{
  if ( m_attachConnection )
  {
    disconnect( m_attachConnection );
    m_attachConnection = QMetaObject::Connection();
  }
  m_view = view;
  m_addItemTool = nullptr;
  if ( !view )
    return;

  m_addItemTool = new QgsLayoutViewToolAddItem( view ); // view owns the tool
  m_attachConnection = connect( this, &PaleoLayoutItemPalette::itemRequested, this,
                                [this]( int metadataId ) {
                                  if ( !m_view || !m_addItemTool || metadataId < 0 )
                                    return;
                                  m_addItemTool->setItemMetadataId( metadataId );
                                  m_view->setTool( m_addItemTool );
                                } );
}

QgsLayoutView *PaleoLayoutItemPalette::attachedView() const
{
  return m_view;
}

QgsLayoutViewToolAddItem *PaleoLayoutItemPalette::addItemTool() const
{
  return m_addItemTool;
}

void PaleoLayoutItemPalette::requestItem( int itemMetadataId )
{
  emit itemRequested( itemMetadataId );
}

void PaleoLayoutItemPalette::requestPageProperties()
{
  emit pagePropertiesRequested();
}

bool PaleoLayoutItemPalette::addItemNow( int itemMetadataId, QgsLayout *layout, QgsLayoutItem **created )
{
  if ( created )
    *created = nullptr;
  if ( !layout || itemMetadataId < 0 )
    return false;

  ensureDefaultItemMetadataRegistered();
  QgsLayoutItemGuiRegistry *registry = QgsGui::layoutItemGuiRegistry();
  QgsLayoutItemAbstractGuiMetadata *metadata = registry ? registry->itemMetadata( itemMetadataId ) : nullptr;
  if ( !metadata )
    return false;

  if ( layout->undoStack() )
    layout->undoStack()->beginMacro( tr( "创建%1" ).arg( localizedItemName( metadata->visibleName() ) ) );

  std::unique_ptr<QgsLayoutItem> item( registry->createItem( itemMetadataId, layout ) );
  if ( !item )
  {
    if ( layout->undoStack() )
      layout->undoStack()->endMacro();
    return false;
  }

  const QRectF pageRect = firstPageRect( layout );
  QSizeF size = defaultSizeFor( metadata );
  size.setWidth( std::min( size.width(), pageRect.width() * 0.9 ) );
  size.setHeight( std::min( size.height(), pageRect.height() * 0.9 ) );
  const QPointF topLeft( pageRect.center().x() - size.width() / 2.0,
                         pageRect.center().y() - size.height() / 2.0 );

  if ( metadata->isNodeBased() )
  {
    // Node items (polygon/polyline) derive their bounds from their node
    // list, which the registry factory leaves empty; give them a sensible
    // default geometry instead of interactive node-clicking.
    QPolygonF nodes;
    if ( metadata->type() == QgsLayoutItemRegistry::LayoutPolygon )
    {
      nodes << QPointF( 0, 0 ) << QPointF( size.width(), 0 )
            << QPointF( size.width(), size.height() ) << QPointF( 0, size.height() );
    }
    else
    {
      nodes << QPointF( 0, size.height() ) << QPointF( size.width() / 2.0, 0 )
            << QPointF( size.width(), size.height() );
    }
    if ( auto *nodeItem = dynamic_cast<QgsLayoutNodesItem *>( item.get() ) )
    {
      nodeItem->setNodes( nodes );
      item->attemptMove( QgsLayoutPoint( topLeft.x(), topLeft.y(), Qgis::LayoutUnit::Millimeters ) );
    }
  }
  else
  {
    item->attemptResize( QgsLayoutSize( size.width(), size.height(), Qgis::LayoutUnit::Millimeters ) );
    item->attemptMove( QgsLayoutPoint( topLeft.x(), topLeft.y(), Qgis::LayoutUnit::Millimeters ) );
  }

  // QGIS-native new-item defaults (label text, legend/scale bar map linking,
  // north arrow path — exactly what interactive creation applies).
  registry->newItemAddedToLayout( itemMetadataId, item.get() );

  QgsLayoutItem *released = item.release();

  // Multiframe factories (attribute/manual/text tables, HTML) return a frame
  // that is already in the layout — same guard as QgsLayoutViewToolAddItem.
  if ( released->scene() != layout )
    layout->addLayoutItem( released );
  layout->setSelectedItem( released );

  if ( layout->undoStack() )
    layout->undoStack()->endMacro();

  if ( created )
    *created = released;
  return true;
}

QWidget *PaleoLayoutItemPalette::buildGroup( const QString &objectName, const QString &title,
                                             const QList<ButtonSpec> &specs )
{
  auto *group = new QWidget( this );
  group->setObjectName( objectName );

  auto *vbox = new QVBoxLayout( group );
  vbox->setContentsMargins( 0, 0, 0, 0 );
  vbox->setSpacing(PaleoTheme::tokens().spacingXs); // DESIGN.md spacing.xs

  auto *caption = new QLabel( title, group );
  QFont captionFont = caption->font();
  captionFont.setPointSizeF(PaleoTheme::tokens().labelPt); // DESIGN.md typography.label (pointSize, DPI-aware)
  caption->setFont( captionFont );
  // DESIGN.md text-muted——PaleoTheme 现取（随主题翻转，活体注册）。
  PaleoTheme::applyThemedStyleSheet( caption, [] { return PaleoTheme::mutedCaptionStyleSheet(); } );
  vbox->addWidget( caption );

  auto *grid = new QGridLayout();
  grid->setSpacing(PaleoTheme::tokens().spacingXs); // DESIGN.md spacing.xs
  vbox->addLayout( grid );

  const int columns = 3;
  int row = 0, column = 0;
  for ( const ButtonSpec &spec : specs )
  {
    const int metadataId = resolveMetadataId( spec.coreType,
                                              spec.variantName ? QString::fromLatin1( spec.variantName )
                                                               : QString() );
    auto *button = new QToolButton( group );
    button->setObjectName( QString::fromLatin1( spec.objectName ) );
    button->setToolButtonStyle( Qt::ToolButtonTextUnderIcon );

    if ( metadataId < 0 )
    {
      // Unavailable in this QGIS build: disabled, with the reason visible
      // (DESIGN.md: disabled tools must carry a reason tooltip).
      button->setText( localizedItemName( QString::fromLatin1( spec.fallbackName ) ) );
      button->setEnabled( false );
      button->setToolTip( tr( "当前 QGIS 运行时不支持此元素类型。" ) );
    }
    else
    {
      QgsLayoutItemAbstractGuiMetadata *metadata = QgsGui::layoutItemGuiRegistry()->itemMetadata( metadataId );
      button->setText( localizedItemName( metadata->visibleName() ) );
      button->setIcon( PaleoIcons::themed(metadata->creationIcon()) );
      button->setToolTip( tr( "添加%1" ).arg( localizedItemName( metadata->visibleName() ) ) );
      const int emittedId = metadataId;
      connect( button, &QToolButton::clicked, this,
               [this, emittedId]() { emit itemRequested( emittedId ); } );
    }

    grid->addWidget( button, row, column );
    m_buttonMetadataIds.append( metadataId );
    if ( ++column == columns )
    {
      column = 0;
      ++row;
    }
  }
  return group;
}

int PaleoLayoutItemPalette::resolveMetadataId( int coreType, const QString &variantName ) const
{
  QgsLayoutItemGuiRegistry *registry = QgsGui::layoutItemGuiRegistry();
  if ( !registry )
    return -1;

  if ( !variantName.isEmpty() )
  {
    // Untranslated match against QGIS's default registration names. If QGIS
    // translation catalogs are loaded into the process, fall back to the
    // first metadata of the core type below (documented deviation).
    const QList<int> ids = registry->itemMetadataIds();
    for ( int id : ids )
    {
      QgsLayoutItemAbstractGuiMetadata *metadata = registry->itemMetadata( id );
      if ( metadata && metadata->type() == coreType && metadata->visibleName() == variantName )
        return id;
    }
  }
  return registry->metadataIdForItemType( coreType );
}
