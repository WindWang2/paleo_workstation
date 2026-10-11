// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <memory>
#include <qgsfeature.h>
#include <qgsgeometry.h>

class QWidget;
class QgsMapCanvas;
class QgsMessageBar;
class QgsVectorLayer;
class QgsVectorLayerTools;
class QgsClipboard;
class QgsFeature;
class QgsAttributeEditorContext;
class QgisEditingService;

// The original QGIS attribute dialog's application entry points, supplied by
// the embedded canvas and the project's edit service instead of QgisApp.
class PaleoAttributeTableHost : public QObject
{
  Q_OBJECT
public:
  PaleoAttributeTableHost(QgsMapCanvas *canvas, QWidget *parent);
  ~PaleoAttributeTableHost() override;
  void setEditingService(QgisEditingService *service);
  QString styleSheet() const;
  QgsMapCanvas *mapCanvas() const;
  QgsMessageBar *messageBar() const;
  QgsAttributeEditorContext createAttributeEditorContext() const;
  QgsClipboard *clipboard() const;
  bool startEditing(QgsVectorLayer *layer);
  bool toggleEditing(QgsVectorLayer *layer);
  bool saveEdits(QgsVectorLayer *layer, bool leaveEditable = true, bool = true);
  bool stopEditing(QgsVectorLayer *layer, bool allowCancel = true);
  void deleteSelected(QgsVectorLayer *layer, QWidget *parent);
  void cutSelectionToClipboard(QgsVectorLayer *layer);
  void copySelectionToClipboard(QgsVectorLayer *layer);
  void pasteFromClipboard(QgsVectorLayer *layer);
  QgsFeature addFeature(QgsVectorLayer *layer, bool showForm, QWidget *parent,
                        const QgsAttributeMap &attributes = {}, const QgsGeometry &geometry = {});
signals:
  void errorOccurred(const QString &reason);
private:
  bool fail(const QString &message);
  QPointer<QgsMapCanvas> m_canvas;
  QPointer<QgisEditingService> m_editingService;
  QPointer<QWidget> m_parent;
  QgsMessageBar *m_messageBar;
  std::unique_ptr<QgsVectorLayerTools> m_tools;
};
