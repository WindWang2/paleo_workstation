#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>

class QgsMapCanvas;
class QgsMapTool;
class QWidget;

// P0 spine service — owns the QgsMapCanvas and map-tool lifecycle.
// Esc deactivates current tool (§42.15); re-entrancy guard on selection broadcasts (§41.3).
class QgisCanvasController : public QObject
{
  Q_OBJECT
  public:
    explicit QgisCanvasController(QObject *parent = nullptr);
    ~QgisCanvasController() override;

    QgsMapCanvas *canvas();                       // created lazily; QWidget* for embedding
    void setMapTool(QgsMapTool *tool);            // deactivates previous
    QgsMapTool *activeTool() const;
    void deactivateTool();                        // Esc path

    void zoomToFullExtent();
    void zoomToLayer(const QString &layerId);     // via QgisLayerService lookup (wired in app)

    // Selection broadcast guard (§41.3): while a broadcast is in flight, incoming
    // selection echoes are swallowed; a coalesced re-broadcast fires at settle.
    void beginSelectionBroadcast();
    void endSelectionBroadcast();
    bool broadcasting() const { return m_broadcasting; }

  signals:
    void selectionBroadcast(const QStringList &featureIds, const QString &origin);

  private:
    QgsMapCanvas *m_canvas = nullptr;
    QgsMapTool *m_tool = nullptr;
    bool m_broadcasting = false;
};
