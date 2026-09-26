#include "attributetablepanel.h"

#include <qgsattributetablemodel.h>
#include <qgsattributetablefiltermodel.h>
#include <qgsattributetableview.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayercache.h>

#include <QComboBox>
#include <QLabel>
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
  setAccessibleName(QStringLiteral("属性表面板"));
  setAccessibleDescription(QStringLiteral("查看所选图层的要素属性表"));
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(6, 6, 6, 6);
  lay->setSpacing(4);

  auto *picker = new QComboBox(this);
  picker->setObjectName(QStringLiteral("attrLayerPicker"));
  picker->setAccessibleName(QStringLiteral("属性表图层选择"));
  lay->addWidget(picker);

  auto *hint = new QLabel(QStringLiteral("（选择图层查看属性表）"), this);
  hint->setObjectName(QStringLiteral("attrEmptyHint"));
  lay->addWidget(hint);

  auto *view = new QgsAttributeTableView(this);
  view->setObjectName(QStringLiteral("attrView"));
  view->setAccessibleName(QStringLiteral("属性表"));
  lay->addWidget(view, 1);

  connect(picker, &QComboBox::activated, this,
          [this](int idx) {
            auto *pickerW = findChild<QComboBox *>(QStringLiteral("attrLayerPicker"));
            if (pickerW)
              showLayer(pickerW->itemData(idx).toString());
          });
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
      hint->setText(QStringLiteral("（图层未实例化或非矢量层：%1）").arg(layerId));
      hint->setVisible(true);
    }
    return;
  }

  clearTable();
  m_cache = new QgsVectorLayerCache(vl, vl->featureCount() > 0
                                          ? static_cast<int>(vl->featureCount()) : 1, view);
  m_model = new QgsAttributeTableModel(m_cache, view);
  m_model->loadLayer();
  m_filter = new QgsAttributeTableFilterModel(m_canvas, m_model, view);
  view->setModel(m_filter);
  if (hint)
    hint->setVisible(false);
}

QString AttributeTablePanel::currentLayerId() const
{
  const auto *picker = findChild<QComboBox *>(QStringLiteral("attrLayerPicker"));
  return picker ? picker->currentData().toString() : QString();
}
