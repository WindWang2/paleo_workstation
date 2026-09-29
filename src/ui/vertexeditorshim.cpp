// 层：视图
#include "vertexeditorshim.h"
#include "paleotheme.h"

#include <QDockWidget>
#include <QHeaderView>
#include <QWidget>

#include <qgsmapcanvas.h>
#include <qgsmaptool.h>
#include <qgspointxy.h>

// ------------------------------------------------- minimal vertex table

PaleoVertexEditorWidget::PaleoVertexEditorWidget( QWidget *parent )
  : QTableWidget( 0, 3, parent )
{
  setHorizontalHeaderLabels( { tr( "#" ), tr( "x" ), tr( "y" ) } );
  setAccessibleName( tr( "顶点坐标表" ) );
  horizontalHeader()->setStretchLastSection( true );
  verticalHeader()->hide();
}

void PaleoVertexEditorWidget::setVertices( const QList<QgsPointXY> &points )
{
  setRowCount( points.size() );
  for ( int i = 0; i < points.size(); ++i )
  {
    const QgsPointXY &pt = points.at( i );
    // 坐标是展示数据，编辑改动会被静默丢弃——直接只读，不假装可编辑。
    auto *idxItem = new QTableWidgetItem( QString::number( i ) );
    auto *xItem = new QTableWidgetItem( QString::number( pt.x(), 'f', 6 ) );
    auto *yItem = new QTableWidgetItem( QString::number( pt.y(), 'f', 6 ) );
    for ( QTableWidgetItem *it : { idxItem, xItem, yItem } )
      it->setFlags( it->flags() & ~Qt::ItemIsEditable );
    // 数值/坐标列用等宽字体（DESIGN.md「数值/坐标/深度」规则）。
    xItem->setFont( PaleoTheme::monoFont() );
    yItem->setFont( PaleoTheme::monoFont() );
    setItem( i, 0, idxItem );
    setItem( i, 1, xItem );
    setItem( i, 2, yItem );
  }
}

void PaleoVertexEditorWidget::clearVertices()
{
  setRowCount( 0 );
}

// ------------------------------------------------------------ shim proper

PaleoVertexEditorShim::PaleoVertexEditorShim( QgsMapCanvas *canvas, QObject *parent )
  : QObject( parent ? parent : static_cast<QObject *>( canvas ) )
  , mCanvas( canvas )
{
  if ( mCanvas )
  {
    connect( mCanvas, &QgsMapCanvas::mapToolSet,
             this, &PaleoVertexEditorShim::handleMapToolSet );
  }
}

void PaleoVertexEditorShim::attachToTool( QgsMapTool *tool )
{
  mAttachedTool = tool;
  // autoShow: reflect the tool's current activation state immediately.
  if ( mAutoShow )
  {
    if ( tool && mCanvas && mCanvas->mapTool() == tool )
      showEditor();
    else if ( mDock )
      hideEditor();
  }
}

QWidget *PaleoVertexEditorShim::editor()
{
  dock(); // ensures mEditor exists inside the dock
  return mEditor;
}

QDockWidget *PaleoVertexEditorShim::dock()
{
  if ( !mDock )
  {
    QDockWidget *dock = new QDockWidget( tr( "顶点编辑器" ), mDockParent );
    dock->setObjectName( QStringLiteral( "paleoVertexEditorDock" ) );
    mEditor = new PaleoVertexEditorWidget( dock );
    dock->setWidget( mEditor );
    dock->hide(); // visible only via showEditor() / autoShow
    mDock = dock;
  }
  return mDock.data();
}

void PaleoVertexEditorShim::setDockParent( QWidget *parent )
{
  mDockParent = parent;
  if ( mDock )
    mDock->setParent( parent ); // reparent; keeps widget + hidden state
}

void PaleoVertexEditorShim::setAutoShow( bool enabled )
{
  if ( mAutoShow == enabled )
    return;
  mAutoShow = enabled;
  if ( enabled && mAttachedTool && mCanvas && mCanvas->mapTool() == mAttachedTool )
    showEditor();
}

void PaleoVertexEditorShim::showEditor()
{
  dock()->show();
}

void PaleoVertexEditorShim::hideEditor()
{
  if ( mDock )
    mDock->hide();
}

void PaleoVertexEditorShim::handleMapToolSet( QgsMapTool *newTool, QgsMapTool *oldTool )
{
  if ( !mAutoShow || !mAttachedTool )
    return;
  if ( newTool == mAttachedTool )
    showEditor();
  else if ( oldTool == mAttachedTool )
    hideEditor();
}
