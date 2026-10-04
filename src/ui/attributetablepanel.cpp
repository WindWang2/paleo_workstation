// 层：视图
#include "paleotheme.h"
#include "attributetablepanel.h"

#include "../qgis/qgiseditingservice.h"

#include <qgsattributetablemodel.h>
#include <qgsattributetablefiltermodel.h>
#include <qgsattributetableview.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayercache.h>

#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QToolButton>
#include <QVBoxLayout>

// Header fixes members to canvas + provider — table internals (cache, source
// model, filter model, view) are resolved via objectName or carried as
// children of the panel. The cache/models must be re-created per layer switch;
// they live as QObject children of the view's model chain and get deleted with
// the panel (old ones deleted explicitly on switch).
AttributeTablePanel::AttributeTablePanel(
    QgsMapCanvas *canvas,
    std::function<QgsVectorLayer *(const QString &)> layerProvider,
    QWidget *parent)
  : QWidget(parent)
  , m_canvas(canvas)
  , m_layerProvider(std::move(layerProvider))
{
  setObjectName(QStringLiteral("attributeTablePanel"));
  // T32 a11y：面板/选择器/表各自报名。
  setAccessibleName(tr("属性表面板"));
  setAccessibleDescription(tr("查看所选图层的要素属性表"));
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  lay->setSpacing(PaleoTheme::tokens().spacingXs);

  auto *picker = new QComboBox(this);
  picker->setObjectName(QStringLiteral("attrLayerPicker"));
  picker->setAccessibleName(tr("属性表图层选择"));
  lay->addWidget(picker);
  buildEditRow();

  auto *hint = new QLabel(tr("（选择图层查看属性表）"), this);
  hint->setObjectName(QStringLiteral("attrEmptyHint"));
  lay->addWidget(hint);

  auto *view = new QgsAttributeTableView(this);
  view->setObjectName(QStringLiteral("attrView"));
  view->setAccessibleName(tr("属性表"));
  lay->addWidget(view, 1);

  connect(picker, &QComboBox::activated, this,
          [this](int idx) {
            auto *pickerW = findChild<QComboBox *>(QStringLiteral("attrLayerPicker"));
            if (pickerW)
              showLayer(pickerW->itemData(idx).toString());
          });
  updateEditRowStates();
}

void AttributeTablePanel::setEditingService(QgisEditingService *service)
{
  m_editingService = service;
  updateEditRowStates();
}

void AttributeTablePanel::buildEditRow()
{
  // 显式编辑入口（主线4）：与编辑工具条同语义的会话三键。原生 QToolBar/
  // QToolButton 风格、纯文本（面板内次级操作，不占主图标面）。
  auto *row = new QWidget(this);
  row->setObjectName(QStringLiteral("attrEditRow"));
  auto *lay = new QHBoxLayout(row);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(PaleoTheme::tokens().spacingXs);

  auto mkButton = [this, row](const QString &objectName, const QString &text,
                              bool (AttributeTablePanel::*slot)()) {
    auto *btn = new QToolButton(row);
    btn->setObjectName(objectName);
    btn->setText(text);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(btn, &QToolButton::clicked, this, [this, slot] { (this->*slot)(); });
    return btn;
  };
  lay->addWidget(mkButton(QStringLiteral("attrEditStartButton"), tr("编辑"),
                          &AttributeTablePanel::beginEditing));
  lay->addWidget(mkButton(QStringLiteral("attrEditSaveButton"), tr("保存编辑"),
                          &AttributeTablePanel::saveEditing));
  lay->addWidget(mkButton(QStringLiteral("attrEditCancelButton"), tr("放弃编辑"),
                          &AttributeTablePanel::cancelEditing));
  lay->addStretch(1);

  auto *panelLay = layout();
  if (panelLay && panelLay->count() >= 1)
    panelLay->addWidget(row); // picker 之后、hint 之前无必要——直接追加即可
}

QgsVectorLayer *AttributeTablePanel::currentLayer() const
{
  const QString id = currentLayerId();
  return m_layerProvider ? m_layerProvider(id) : nullptr;
}

bool AttributeTablePanel::isEditing() const
{
  QgsVectorLayer *vl = currentLayer();
  return vl && vl->isEditable();
}

bool AttributeTablePanel::beginEditing()
{
  QgsVectorLayer *vl = currentLayer();
  if (!vl)
  {
    emit editRefused(tr("属性表：当前没有可编辑的图层"));
    return false;
  }
  if (vl->isEditable())
    return true; // 既有会话（本面板或工具条开的）——幂等采用
  if (!vl->supportsEditing() || vl->readOnly())
  {
    emit editRefused(tr("属性表：图层 %1 不支持编辑").arg(vl->name()));
    return false;
  }

  QString err;
  const bool ok = m_editingService ? m_editingService->beginEdit(vl, &err)
                                   : vl->startEditing();
  if (!ok)
  {
    emit editRefused(m_editingService ? err
                                      : tr("属性表：无法开始编辑图层 %1").arg(vl->name()));
    return false;
  }
  emit editingStarted(vl->id());
  updateEditRowStates();
  return true;
}

bool AttributeTablePanel::saveEditing()
{
  QgsVectorLayer *vl = currentLayer();
  if (!vl || !vl->isEditable())
  {
    emit editRefused(tr("属性表：当前没有进行中的编辑会话"));
    return false;
  }
  // 提交语义与 MapVersionController 一致：走服务（enqueueWrite 单写者 +
  // markLayerFree）；无服务时直连 commitChanges（native 清 undo 栈同效）。
  QString err;
  const bool ok = m_editingService ? m_editingService->commitEdit(vl, &err)
                                   : vl->commitChanges();
  if (!ok)
  {
    emit editRefused(m_editingService ? err
                                      : tr("属性表：提交图层 %1 的编辑失败").arg(vl->name()));
    return false;
  }
  emit editingStopped(vl->id(), true);
  updateEditRowStates();
  return true;
}

bool AttributeTablePanel::cancelEditing()
{
  QgsVectorLayer *vl = currentLayer();
  if (!vl || !vl->isEditable())
  {
    emit editRefused(tr("属性表：当前没有进行中的编辑会话"));
    return false;
  }
  // C4 数据安全：rollBack 丢弃整个编辑会话——有未提交修改时先确认。
  // offscreen（测试/CI）无窗口系统不弹框（硬纪律），直走回滚。
  if (vl->isModified() &&
      QGuiApplication::platformName() != QLatin1String("offscreen"))
  {
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("放弃编辑"),
        tr("图层「%1」有未提交的修改——放弃后将全部丢失，确定放弃？")
            .arg(vl->name()),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard)
      return false; // 用户取消：会话原样保留
  }
  const bool ok = m_editingService ? m_editingService->rollbackEdit(vl)
                                   : vl->rollBack();
  if (!ok)
  {
    emit editRefused(tr("属性表：回滚图层 %1 失败").arg(vl->name()));
    return false;
  }
  emit editingStopped(vl->id(), false);
  updateEditRowStates();
  return true;
}

void AttributeTablePanel::updateEditRowStates()
{
  QgsVectorLayer *vl = currentLayer();
  auto *start = findChild<QToolButton *>(QStringLiteral("attrEditStartButton"));
  auto *save = findChild<QToolButton *>(QStringLiteral("attrEditSaveButton"));
  auto *cancel = findChild<QToolButton *>(QStringLiteral("attrEditCancelButton"));
  if (!start || !save || !cancel)
    return;
  const bool hasLayer = vl != nullptr;
  const bool editing = vl && vl->isEditable();
  const bool canStart = hasLayer && !editing && vl->supportsEditing() && !vl->readOnly();
  start->setEnabled(canStart);
  start->setToolTip(canStart || !hasLayer
                        ? (hasLayer ? tr("开始编辑当前图层") : tr("先选择图层"))
                        : tr("图层 %1 不支持编辑").arg(vl->name()));
  save->setEnabled(editing);
  save->setToolTip(editing ? tr("提交当前图层的编辑（进版本管线）")
                           : tr("当前没有进行中的编辑会话"));
  cancel->setEnabled(editing);
  cancel->setToolTip(editing ? tr("放弃当前图层的编辑")
                             : tr("当前没有进行中的编辑会话"));
}

void AttributeTablePanel::setLayerIds(const QStringList &ids)
{
  auto *picker = findChild<QComboBox *>(QStringLiteral("attrLayerPicker"));
  if (!picker)
    return;
  const QString keep = picker->currentData().toString();
  picker->clear();
  for (const QString &id : ids)
    picker->addItem(id, id);
  const int idx = picker->findData(keep);
  if (idx >= 0)
    picker->setCurrentIndex(idx);
}

void AttributeTablePanel::clearTable()
{
  auto *view = findChild<QgsAttributeTableView *>(QStringLiteral("attrView"));
  if (view)
  {
    // Drop the view's first-layer selection manager before the model goes
    // away. setModel() keeps that manager forever if it is already set.
    view->setModel(nullptr);
    view->setFeatureSelectionManager(nullptr);
  }
  delete m_filter;
  m_filter = nullptr;
  delete m_model;
  m_model = nullptr;
  delete m_cache;
  m_cache = nullptr;

  auto *hint = findChild<QLabel *>(QStringLiteral("attrEmptyHint"));
  if (hint)
  {
    hint->setText(tr("（选择图层查看属性表）"));
    hint->setVisible(true);
  }
}

void AttributeTablePanel::showLayer(const QString &layerId)
{
  auto *picker = findChild<QComboBox *>(QStringLiteral("attrLayerPicker"));
  auto *view = findChild<QgsAttributeTableView *>(QStringLiteral("attrView"));
  auto *hint = findChild<QLabel *>(QStringLiteral("attrEmptyHint"));
  if (!picker || !view)
    return;

  const int idx = picker->findData(layerId);
  if (idx >= 0)
    picker->setCurrentIndex(idx);

  QgsVectorLayer *vl = m_layerProvider ? m_layerProvider(layerId) : nullptr;
  if (!vl)
  {
    clearTable();
    if (hint)
    {
      hint->setText(tr("（图层未实例化或非矢量层：%1）").arg(layerId));
      hint->setVisible(true);
    }
    updateEditRowStates();
    return;
  }

  clearTable();
  connect(vl, &QObject::destroyed, this, &AttributeTablePanel::clearTable, Qt::UniqueConnection);
  // 会话状态可能被外部（编辑工具条/版本控制器）改变——按钮跟随。
  connect(vl, &QgsVectorLayer::editingStarted, this, &AttributeTablePanel::updateEditRowStates,
          Qt::UniqueConnection);
  connect(vl, &QgsVectorLayer::editingStopped, this, &AttributeTablePanel::updateEditRowStates,
          Qt::UniqueConnection);
  m_cache = new QgsVectorLayerCache(vl, vl->featureCount() > 0
                                          ? static_cast<int>(vl->featureCount()) : 1, view);
  m_model = new QgsAttributeTableModel(m_cache, view);
  m_model->loadLayer();
  m_filter = new QgsAttributeTableFilterModel(m_canvas, m_model, view);
  view->setModel(m_filter);
  if (hint)
    hint->setVisible(false);
  updateEditRowStates();
}

QString AttributeTablePanel::currentLayerId() const
{
  const auto *picker = findChild<QComboBox *>(QStringLiteral("attrLayerPicker"));
  return picker ? picker->currentData().toString() : QString();
}
