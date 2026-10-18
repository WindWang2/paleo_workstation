// 层：QGIS 封装
#include "nativeattributetablehost.h"
#include "qgiseditingservice.h"
#include "qgsclipboard.h"
#include <qgsattributedialog.h>
#include <qgsattributeeditorcontext.h>
#include <qgsmapcanvas.h>
#include <qgsmessagebar.h>
#include <qgsgui.h>
#include <qgseditorwidgetregistry.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayertools.h>
#include <qgsvectorlayerutils.h>
#include <QApplication>
#include <QMessageBox>

namespace {
class AttributeLayerTools final : public QgsVectorLayerTools
{
public:
  explicit AttributeLayerTools(PaleoAttributeTableHost *host) : m_host(host) {}
  bool startEditing(QgsVectorLayer *layer) const override { return m_host->startEditing(layer); }
  bool stopEditing(QgsVectorLayer *layer, bool allowCancel = true) const override
  { return m_host->stopEditing(layer, allowCancel); }
  bool saveEdits(QgsVectorLayer *layer) const override { return m_host->saveEdits(layer); }
  bool addFeature(QgsVectorLayer *layer, const QgsAttributeMap &attributes,
                  const QgsGeometry &geometry, QgsFeature *feature, QWidget *parent,
                  bool, bool) const override
  {
    const auto result = m_host->addFeature(layer, true, parent, attributes, geometry);
    if (feature) *feature = result;
    return result.isValid();
  }
private:
  PaleoAttributeTableHost *m_host;
};
}

PaleoAttributeTableHost::PaleoAttributeTableHost(QgsMapCanvas *canvas, QWidget *parent)
  : QObject(parent), m_canvas(canvas), m_parent(parent),
    m_messageBar(new QgsMessageBar(parent)), m_tools(std::make_unique<AttributeLayerTools>(this))
{
  // Also support hosts that initialize QgsApplication without QgisRuntime.
  auto *registry = QgsGui::editorWidgetRegistry();
  if (!registry->factories().contains(QStringLiteral("TextEdit")))
    registry->initEditors(canvas, m_messageBar);
  m_messageBar->hide();
}
PaleoAttributeTableHost::~PaleoAttributeTableHost() = default;
void PaleoAttributeTableHost::setEditingService(QgisEditingService *service) { m_editingService = service; }
QString PaleoAttributeTableHost::styleSheet() const { return m_parent ? m_parent->styleSheet() : QString(); }
QgsMapCanvas *PaleoAttributeTableHost::mapCanvas() const { return m_canvas; }
QgsMessageBar *PaleoAttributeTableHost::messageBar() const { return m_messageBar; }
QgsAttributeEditorContext PaleoAttributeTableHost::createAttributeEditorContext() const
{
  QgsAttributeEditorContext context;
  context.setMapCanvas(m_canvas);
  context.setVectorLayerTools(m_tools.get());
  return context;
}
QgsClipboard *PaleoAttributeTableHost::clipboard() const
{
  static QPointer<QgsClipboard> shared;
  if (!shared) shared = new QgsClipboard();
  if (!shared->parent()) shared->setParent(qApp);
  return shared;
}
bool PaleoAttributeTableHost::fail(const QString &message)
{
  m_messageBar->pushMessage(tr("属性表"), message, Qgis::MessageLevel::Warning);
  emit errorOccurred(message);
  return false;
}
bool PaleoAttributeTableHost::startEditing(QgsVectorLayer *layer)
{
  if (!layer) return false;
  if (layer->isEditable()) return true;
  QString error;
  const bool ok = m_editingService ? m_editingService->beginEdit(layer, &error) : layer->startEditing();
  return ok || fail(error.isEmpty() ? tr("无法开始编辑图层「%1」").arg(layer->name()) : error);
}
bool PaleoAttributeTableHost::saveEdits(QgsVectorLayer *layer, bool leaveEditable, bool)
{
  if (!layer || !layer->isEditable()) return false;
  QString error;
  const bool ok = m_editingService ? m_editingService->commitEdit(layer, &error) : layer->commitChanges();
  if (!ok) {
    if (error.isEmpty()) error = layer->commitErrors().join(QLatin1Char('\n'));
    return fail(error.isEmpty() ? tr("保存图层编辑失败") : error);
  }
  return !leaveEditable || startEditing(layer);
}
bool PaleoAttributeTableHost::stopEditing(QgsVectorLayer *layer, bool allowCancel)
{
  if (!layer || !layer->isEditable()) return true;
  if (layer->isModified()) {
    // R2-SF-04：seam 先于 offscreen 早退判定——注入的 ask 是显式契约（含
    // 测试驱动），无头环境同样生效；未注入时 offscreen 保持直存语义。
    if (m_saveDiscardAsk && allowCancel) {
      const auto choice = m_saveDiscardAsk(layer->name());
      if (choice == SaveChoice::Cancel) return false;
      if (choice == SaveChoice::Save) return saveEdits(layer, false);
    } else if (QGuiApplication::platformName() == QLatin1String("offscreen") || !m_parent) {
      return saveEdits(layer, false);
    } else {
      const auto answer = QMessageBox::question(m_parent, tr("保存编辑"),
        tr("是否保存图层「%1」的修改？").arg(layer->name()),
        QMessageBox::Save | QMessageBox::Discard | (allowCancel ? QMessageBox::Cancel : QMessageBox::NoButton),
        QMessageBox::Save);
      if (answer == QMessageBox::Cancel) return false;
      if (answer == QMessageBox::Save) return saveEdits(layer, false);
    }
  }
  const bool ok = m_editingService ? m_editingService->rollbackEdit(layer) : layer->rollBack();
  return ok || fail(tr("放弃图层编辑失败"));
}
bool PaleoAttributeTableHost::toggleEditing(QgsVectorLayer *layer)
{
  return layer && (layer->isEditable() ? stopEditing(layer) : startEditing(layer));
}
void PaleoAttributeTableHost::deleteSelected(QgsVectorLayer *layer, QWidget *)
{
  if (!layer || !layer->isEditable() || layer->selectedFeatureIds().isEmpty()) return;
  layer->beginEditCommand(tr("删除所选要素"));
  int deleted = 0;
  if (layer->deleteSelectedFeatures(&deleted)) layer->endEditCommand();
  else { layer->destroyEditCommand(); fail(tr("删除所选要素失败")); }
}
void PaleoAttributeTableHost::copySelectionToClipboard(QgsVectorLayer *layer)
{ if (layer) clipboard()->replaceWithCopyOf(layer); }
void PaleoAttributeTableHost::cutSelectionToClipboard(QgsVectorLayer *layer)
{ copySelectionToClipboard(layer); deleteSelected(layer, m_parent); }
void PaleoAttributeTableHost::pasteFromClipboard(QgsVectorLayer *layer)
{
  if (!layer || !layer->isEditable()) return;
  QgsFeatureList features;
  try {
    features = QgsVectorLayerUtils::makeFeaturesCompatible(clipboard()->transformedCopyOf(layer->crs(), layer->fields()), layer);
  } catch (const QgsCsException &error) { fail(error.what()); return; }
  if (features.isEmpty()) return;
  layer->beginEditCommand(tr("粘贴要素"));
  if (layer->addFeatures(features)) {
    layer->endEditCommand();
    QgsFeatureIds ids;
    for (const auto &feature : features) ids.insert(feature.id());
    layer->selectByIds(ids);
  } else { layer->destroyEditCommand(); fail(tr("粘贴要素失败")); }
}
QgsFeature PaleoAttributeTableHost::addFeature(QgsVectorLayer *layer, bool showForm, QWidget *parent,
                                             const QgsAttributeMap &attributes, const QgsGeometry &geometry)
{
  if (!layer || !layer->isEditable()) return {};
  auto context = layer->createExpressionContext();
  auto feature = QgsVectorLayerUtils::createFeature(layer, geometry, attributes, &context);
  if (showForm && QGuiApplication::platformName() != QLatin1String("offscreen") && parent) {
    QgsAttributeDialog dialog(layer, &feature, false, parent, true, createAttributeEditorContext());
    dialog.setMode(QgsAttributeEditorContext::AddFeatureMode);
    if (dialog.exec() != QDialog::Accepted) return {};
    return *dialog.feature();
  }
  layer->beginEditCommand(tr("新增要素"));
  if (layer->addFeature(feature)) { layer->endEditCommand(); return feature; }
  layer->destroyEditCommand();
  fail(tr("新增要素失败"));
  return {};
}
