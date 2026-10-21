// 层：视图
// 测区全景在线底图切换（无 / 街道 / 卫星 / 地形 / 自定义源）。QGIS 细节全在
// src/qgis/onlinebasemap 封装里；本文件只做控件与接线。
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"
#include "previewmappage.h"

#include "../../qgis/onlinebasemap.h"
#include "../../qgis/projectmapreference.h"

#include <QComboBox>
#include <QLabel>
#include <QPointer>

namespace {
constexpr int kTileTimeoutMs = 15000;
}

void DataPreviewTabs::attachOnlineBasemap(QWidget *content)
{
  namespace ob = paleo::onlinebasemap;
  auto *mapPage = content->findChild<PreviewMapPage *>(QStringLiteral("surveyPreviewPage"));
  if (!mapPage) return;
  auto *canvas = mapPage->mapCanvas()->canvas();
  auto *combo = new QComboBox(mapPage);
  combo->setObjectName(QStringLiteral("surveyOnlineBasemap"));
  combo->setAccessibleName(tr("在线底图"));
  combo->addItem(tr("底图：无"), ob::noneId());
  for (const auto &source : ob::sources())
    combo->addItem(tr("底图：%1").arg(source.title), source.id);
  mapPage->addToolBarWidget(combo);

  QString reason;
  const bool usable = m_project && ob::canOverlay(canvas, &reason);
  if (!usable) {
    combo->setEnabled(false);
    combo->setToolTip(reason.isEmpty() ? tr("未打开工程，无法叠加在线底图") : reason);
    return;
  }
  auto *status = content->findChild<QLabel *>(QStringLiteral("surveyBasemapAttribution"));
  if (!status) {
    status = new QLabel(content);
    status->setObjectName(QStringLiteral("surveyBasemapAttribution"));
    status->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(status, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    if (auto *box = qobject_cast<QBoxLayout *>(content->layout())) box->addWidget(status);
  }

  combo->setToolTip(tr("叠加开源免费在线底图（需联网）；工程自带离线底图时默认不叠加。"
                       "自定义瓦片源见设置 basemap/onlineSources"));

  auto project = m_project; // QPointer 拷贝
  auto apply = [mapPage, canvas, status, project](const QString &id) {
    if (auto *old = ob::findLayer(mapPage->mapCanvas()->layers())) {
      mapPage->removeMapLayer(old);
      ob::destroyLayer(old);
    }
    if (project) ob::saveChoice(project, id);
    const auto source = ob::sourceById(id);
    auto *layer = ob::createLayer(source, mapPage);
    if (layer) {
      mapPage->addMapLayerAtBottom(layer, tr("在线底图：%1").arg(source.title));
      QPointer<QLabel> label = status;
      ob::watchFailures(canvas, layer, kTileTimeoutMs, [label](const QString &message) {
        if (label) label->setText(message);
      });
    } else if (!source.id.isEmpty()) {
      status->setText(tr("在线底图源无效：%1").arg(source.title));
      return;
    }
    status->setText(paleo::mapreference::attribution(canvas));
  };
  QString saved = ob::savedChoice(m_project);
  const int index = combo->findData(saved);
  combo->setCurrentIndex(index < 0 ? 0 : index);
  connect(combo, &QComboBox::currentIndexChanged, mapPage,
          [combo, apply](int) { apply(combo->currentData().toString()); });
  if (combo->currentIndex() > 0) apply(combo->currentData().toString());
}
