// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
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
  // FD-004：破坏性确认（保存/放弃/取消）的 ask seam——DESIGN.md Tier-2 要求
  // 破坏性确认统一走 ui/notifications/paleonotify.h 收敛；qgis 封装层按分层
  // 不能 include ui/，由壳侧注入。缺省（未注入）回落现有原生 QMessageBox
  //（降级契约，同 paleonotify 无 hub 回落）；offscreen 分支保持直存语义。
  // allowCancel=false 的罕见调用保持原生两键路径（不引入 seam 语义漂移）。
  enum class SaveChoice { Save, Discard, Cancel };
  void setSaveDiscardAsk(const std::function<SaveChoice(const QString &layerName)> &ask)
  { m_saveDiscardAsk = ask; }

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
  std::function<SaveChoice(const QString &)> m_saveDiscardAsk;
  std::unique_ptr<QgsVectorLayerTools> m_tools;
};
