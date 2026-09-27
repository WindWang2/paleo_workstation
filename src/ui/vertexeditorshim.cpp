// 层：视图
#include "vertexeditorshim.h"

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
  setHorizontalHeaderLabels( { QStringLiteral( "#" ),
                               QStringLiteral( "x" ),
                               QStringLiteral( "y" ) } );
  horizontalHeader()->setStretchLastSection( true );
  verticalHeader()->hide();
}

void PaleoVertexEditorWidget::setVertices( const QList<QgsPointXY> &points )
{
  setRowCount( points.size() );
  for ( int i = 0; i < points.size(); ++i )
  {
    const QgsPointXY &pt = points.at( i );
    setItem( i, 0, new QTableWidgetItem( QString::number( i ) ) );
    setItem( i, 1, new QTableWidgetItem( QString::number( pt.x(), 'f', 6 ) ) );
    setItem( i, 2, new QTableWidgetItem( QString::number( pt.y(), 'f', 6 ) ) );
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
    QDockWidget *dock = new QDockWidget( tr( "Vertex Editor" ), mDockParent );
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
