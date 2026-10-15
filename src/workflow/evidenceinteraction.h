// 层：功能
#pragma once
#include "../linkage/selectioncontext.h"
#include <QObject>
#include <QPointer>
#include <QVariantMap>
class MappingWorkbench;
class QgisLayerService;
class QgisProjectService;
class QgisCanvasController;
class EvidenceMapAdapter;

// Orchestrates existing selection, mapping queries and QGIS highlighting.
// The SelectionContext owns the primary research target; a source highlight
// never replaces it or enters the native editing session.
class EvidenceInteraction : public QObject {
  Q_OBJECT
public:
  EvidenceInteraction(MappingWorkbench *, SelectionContext *, QgisLayerService *,
                      QgisProjectService *, QgisCanvasController *, QObject *parent = nullptr);
  void setLayer(const QString &layerId);
  void navigate(const QVariantMap &evidence, bool locate = true);
  void navigateGraphNode(const QVariantMap &node);
  void refresh();
  void reset();
  QVariantMap snapshot() const { return m_snapshot; }
signals:
  void detailsChanged(const QVariantMap &snapshot, quint64 generation);
  void navigationFailed(const QString &reason);
private:
  void observe(const QString &layerId);
  void capture(const QString &layerId);
  void receive(const SelectionTarget &, const QString &origin, quint64 generation);
  QString projectId() const;
  QPointer<MappingWorkbench> m_workbench;
  QPointer<SelectionContext> m_selection;
  QPointer<QgisLayerService> m_layers;
  QPointer<QgisProjectService> m_project;
  QPointer<QgisCanvasController> m_canvas;
  EvidenceMapAdapter *m_primary, *m_source;
  QVariantMap m_snapshot;
  bool m_receiving = false;
};
