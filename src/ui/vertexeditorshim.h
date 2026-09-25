#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QTableWidget>

class QDockWidget;
class QWidget;
class QgsMapCanvas;
class QgsMapTool;
class QgsPointXY;

// ui/ — PaleoVertexEditorShim per ET9 audit.
//
// Deviation from plan: upstream QgsVertexEditor IS exported by libqgis_app
// (QgsVertexEditor::QgsVertexEditor(QgsMapCanvas*) exists as a symbol), but it
// is APP_EXPORT — its header is not installed, it is not safe to instantiate
// via a re-declared class (unknown sizeof, QObject layout), and it is coupled
// to QgsLockedFeature / QgsVertexTool which are likewise app-only. There is no
// installed QgsVertexEditor or QgsMapToolVertexEdit/QgsVertexTool header in
// this QGIS 4.2.2 build.
//
// The shim therefore provides the same *plumbing* contract around a minimal
// vertex table widget (PaleoVertexEditorWidget): a lazily-created
// QDockWidget ("Vertex Editor") that reparents into a host QWidget, shows and
// hides with the attached map tool when autoShow() is on, and exposes
// editor(). When upstream QgsVertexEditor is ported (audit: copy
// qgsvertexeditor.* + lockedfeature.* + shim ~9 QgisApp iface calls), the dock
// logic here stays and only the inner widget swaps.

// Minimal vertex list — one row per vertex (id, x, y), populated via
// setVertices(). Placeholder for the real QgsVertexEditor model/table.
class PaleoVertexEditorWidget : public QTableWidget
{
  public:
    explicit PaleoVertexEditorWidget( QWidget *parent = nullptr );
    void setVertices( const QList<QgsPointXY> &points );
    void clearVertices();
};

// Owns the vertex-editor dock + lazy editor for a QgsMapCanvas.
class PaleoVertexEditorShim : public QObject
{
    Q_OBJECT
  public:
    // Parented to \a canvas when \a parent is nullptr (matches upstream
    // QgsVertexEditor, which the canvas lifetime owns).
    explicit PaleoVertexEditorShim( QgsMapCanvas *canvas, QObject *parent = nullptr );

    // Associates the shim with \a tool (any QgsMapTool — the shim is generic;
    // attach nullptr to detach). With autoShow() on, the dock shows while the
    // attached tool is the canvas's active map tool.
    void attachToTool( QgsMapTool *tool );
    QgsMapTool *attachedTool() const { return mAttachedTool; }

    // Lazily creates the editor widget (inside dock()).
    QWidget *editor();
    // Lazily creates the dock, parented to dockParent().
    QDockWidget *dock();
    bool hasDock() const { return !mDock.isNull(); }

    // Host widget the dock is parented to (e.g. the main window). Reparents
    // an already-created dock; nullptr leaves it floating.
    void setDockParent( QWidget *parent );
    QWidget *dockParent() const { return mDockParent; }

    // When true (default), the dock follows attached-tool activation.
    bool autoShow() const { return mAutoShow; }
    void setAutoShow( bool enabled );

    QgsMapCanvas *canvas() const { return mCanvas; }

  public slots:
    void showEditor();
    void hideEditor();

  private slots:
    void handleMapToolSet( QgsMapTool *newTool, QgsMapTool *oldTool );

  private:
    QgsMapCanvas *mCanvas = nullptr;       // not owned; QObject parent
    QgsMapTool *mAttachedTool = nullptr;   // not owned
    QWidget *mDockParent = nullptr;        // not owned
    QPointer<QDockWidget> mDock;           // owned by mDockParent
    PaleoVertexEditorWidget *mEditor = nullptr; // owned by mDock
    bool mAutoShow = true;
};
