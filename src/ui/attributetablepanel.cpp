// 层：视图
#include "paleotheme.h"
#include "attributetablepanel.h"

#include "../qgis/qgiseditingservice.h"
#include "../qgis/nativeattributetablehost.h"
#include "qgsattributetabledialog.h"
#include <qgsdualview.h>
#include <qgsmessagebar.h>

#include <qgsattributetablemodel.h>
#include <qgsattributetablefiltermodel.h>
#include <qgsattributetableview.h>
#include <qgsvectorlayer.h>


#include <QComboBox>
#include <QGuiApplication>

#include <QLabel>
#include "notifications/paleonotify.h"

#include <QVBoxLayout>

// The layer picker hosts the original QGIS dialog; QGIS owns its table,
// filtering, form view, field tools and model lifetimes.
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
  m_host = new PaleoAttributeTableHost(canvas, this);
  // FD-004：破坏性确认统一走 PaleoNotify Tier-2 收敛（DESIGN.md「错误呈现」；
  // 未注入时宿主回落原生 QMessageBox 的降级契约保持）。
  m_host->setSaveDiscardAsk([this](const QString &layerName) -> PaleoAttributeTableHost::SaveChoice {
    switch (PaleoNotify::askSaveDiscard(
        this, PaleoNotify::AskIcon::Question, tr("保存编辑"),
        tr("是否保存图层「%1」的修改？").arg(layerName),
        tr("保存"), tr("放弃"), tr("取消"))) {
      case PaleoNotify::SaveChoice::Save:
        return PaleoAttributeTableHost::SaveChoice::Save;
      case PaleoNotify::SaveChoice::Discard:
        return PaleoAttributeTableHost::SaveChoice::Discard;
      case PaleoNotify::SaveChoice::Cancel:
        break;
    }
    return PaleoAttributeTableHost::SaveChoice::Cancel;
  });
  lay->addWidget(m_host->messageBar());
  connect(m_host, &PaleoAttributeTableHost::errorOccurred, this, &AttributeTablePanel::editRefused);

  auto *hint = new QLabel(tr("（选择图层查看属性表）"), this);
  hint->setObjectName(QStringLiteral("attrEmptyHint"));
  lay->addWidget(hint);

  m_tableHost = new QWidget(this);
  m_tableHost->setObjectName(QStringLiteral("nativeAttributeTableHost"));
  auto *tableLayout = new QVBoxLayout(m_tableHost);
  tableLayout->setContentsMargins(0, 0, 0, 0);
  lay->addWidget(m_tableHost, 1);

  connect(picker, &QComboBox::activated, this,
          [this](int idx) {
            auto *pickerW = findChild<QComboBox *>(QStringLiteral("attrLayerPicker"));
            if (pickerW)
              showLayer(pickerW->itemData(idx).toString());
          });
  updateEditRowStates();
}

AttributeTablePanel::~AttributeTablePanel()
{
  clearTable();
}

void AttributeTablePanel::setEditingService(QgisEditingService *service)
{
  m_editingService = service;
  m_host->setEditingService(service);
  updateEditRowStates();
}

QgsVectorLayer *AttributeTablePanel::currentLayer() const
{
  return m_currentLayer;
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
  // 原生 DualView 的 saveEditChanges 只服务表单视图校验；表格/API 已写入
  // edit buffer 时（isModified）不应被空表单的 false 挡住提交（#344 后
  // tst_attrpanel 与工具条同路径）。
  if (m_nativeDialog) {
    if (auto *dv = m_nativeDialog->dualView()) {
      if (auto *tv = dv->tableView())
        tv->closeCurrentEditor();
      // saveEditChanges() 只有表单视图下才有 QgsAttributeForm 可存；
      // 表格视图下恒返 false，不能用它当保存闸门。
      if (dv->view() == QgsDualView::AttributeEditor && !dv->saveEditChanges()) {
        emit editRefused(tr("属性表：表单验证未通过"));
        return false;
      }
    }
  }
  QString err;
  const bool ok = m_editingService ? m_editingService->commitEdit(vl, &err)
                                   : vl->commitChanges();
  if (!ok)
  {
    const QString validation = vl->customProperty("paleo/wellAttributeValidationError").toString();
    emit editRefused(!validation.isEmpty() ? validation : m_editingService ? err
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
    if (!PaleoNotify::ask(
            this, tr("放弃编辑"),
            tr("图层「%1」有未提交的修改——放弃后将全部丢失，确定放弃？")
                .arg(vl->name()),
            PaleoNotify::AskButtons::DiscardCancel, PaleoNotify::AskDefault::Reject))
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
  if (m_nativeDialog && currentLayer())
    m_nativeDialog->editingToggled();
}

void AttributeTablePanel::setLayerIds(const QStringList &ids, const QStringList &names)
{
  auto *picker = findChild<QComboBox *>(QStringLiteral("attrLayerPicker"));
  if (!picker)
    return;
  const QString keep = m_currentLayerId;
  picker->clear();
  // 下拉显示与图层列表一致的图层名（无名才退回图层 id）；id 留在 data 里。
  for (qsizetype i = 0; i < ids.size(); ++i)
  {
    const QString name = names.value(i).trimmed();
    picker->addItem(name.isEmpty() ? ids.at(i) : name, ids.at(i));
  }
  const int idx = picker->findData(keep);
  if (idx >= 0)
    picker->setCurrentIndex(idx);
}

void AttributeTablePanel::clearTable()
{
  disconnect(m_layerDestroyed);
  m_currentLayer = nullptr;
  m_currentLayerId.clear();
  m_model = nullptr;
  m_filter = nullptr;
  delete m_nativeDialog;
  m_nativeDialog = nullptr;

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
  auto *hint = findChild<QLabel *>(QStringLiteral("attrEmptyHint"));
  if (!picker)
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
  m_currentLayer = vl;
  m_currentLayerId = layerId;
  // 名字以活图层为准（图层列表显示的就是它），改名时跟随。
  const auto syncName = [picker, layerId, vl] {
    const int row = picker->findData(layerId);
    if (row >= 0 && !vl->name().isEmpty())
      picker->setItemText(row, vl->name());
  };
  syncName();
  m_nativeDialog = new QgsAttributeTableDialog(vl, m_host, m_tableHost);
  m_nativeDialog->setObjectName(QStringLiteral("nativeAttributeTableDialog"));
  m_tableHost->layout()->addWidget(m_nativeDialog);
  m_nativeDialog->show();
  m_nativeDialog->dualView()->tableView()->setObjectName(QStringLiteral("attrView"));
  m_nativeDialog->dualView()->tableView()->setAccessibleName(tr("属性表"));
  m_model = m_nativeDialog->dualView()->masterModel();
  m_filter = qobject_cast<QgsAttributeTableFilterModel *>(m_nativeDialog->dualView()->tableView()->model());
  m_layerDestroyed = connect(vl, &QObject::destroyed, this, &AttributeTablePanel::clearTable);
  connect(vl, &QgsMapLayer::nameChanged, m_nativeDialog, syncName);
  if (hint)
    hint->setVisible(false);
  updateEditRowStates();
}

QString AttributeTablePanel::currentLayerId() const
{
  return m_currentLayerId;
}

bool AttributeTablePanel::updateCell(int row, int column, const QVariant &value)
{
  if (!m_filter || row < 0 || column < 0)
    return false;
  const QModelIndex idx = m_filter->index(row, column);
  if (!idx.isValid())
    return false;
  return m_filter->setData(idx, value, Qt::EditRole);
}

int AttributeTablePanel::updateCells(const QVector<std::tuple<int, int, QVariant>> &cellUpdates)
{
  int count = 0;
  for (const auto &[row, col, val] : cellUpdates)
  {
    if (updateCell(row, col, val))
      ++count;
  }
  return count;
}
