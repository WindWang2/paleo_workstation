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
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(6, 6, 6, 6);
  lay->setSpacing(4);

  auto *picker = new QComboBox(this);
  picker->setObjectName(QStringLiteral("attrLayerPicker"));
  lay->addWidget(picker);

  auto *hint = new QLabel(QStringLiteral("（选择图层查看属性表）"), this);
  hint->setObjectName(QStringLiteral("attrEmptyHint"));
  lay->addWidget(hint);

  auto *view = new QgsAttributeTableView(this);
  view->setObjectName(QStringLiteral("attrView"));
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
    view->setModel(nullptr);
    if (hint)
    {
      hint->setText(QStringLiteral("（图层未实例化或非矢量层：%1）").arg(layerId));
      hint->setVisible(true);
    }
    return;
  }

  // Fresh cache + model chain per switch; parents keep them alive and old
  // chains die with their model's parent (the view takes over the filter
  // model in setModel per SIP_TRANSFERTHIS in the header annotation).
  auto *cache = new QgsVectorLayerCache(vl, vl->featureCount() > 0
                                              ? static_cast<int>(vl->featureCount()) : 1, view);
  auto *model = new QgsAttributeTableModel(cache, view);
  model->loadLayer();
  auto *filter = new QgsAttributeTableFilterModel(m_canvas, model, view);
  view->setModel(filter);
  if (hint)
    hint->setVisible(false);
}

QString AttributeTablePanel::currentLayerId() const
{
  const auto *picker = findChild<QComboBox *>(QStringLiteral("attrLayerPicker"));
  return picker ? picker->currentData().toString() : QString();
}
