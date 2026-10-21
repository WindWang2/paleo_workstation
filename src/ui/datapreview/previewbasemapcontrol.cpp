// 层：视图
#include "previewbasemapcontrol.h"

#include "previewmappage.h"
#include "../../qgis/basemaplayer.h"
#include "../../qgis/onlinebasemap.h"
#include "../../qgis/projectmapreference.h"
#include "../paleotheme.h"

#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolButton>

namespace {
constexpr int kTileTimeoutMs = 15000;
namespace bm = paleo::basemap;
namespace ob = paleo::onlinebasemap;
} // namespace

PreviewBasemapControl::PreviewBasemapControl(PreviewMapPage *page, QObject *project,
                                             const QString &prefix, QLabel *status)
    : QWidget(page), m_page(page), m_project(project), m_status(status), m_current(ob::noneId())
{
  setObjectName(prefix + QStringLiteral("BasemapControl"));
  auto *lay = new QHBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(PaleoTheme::tokens().spacingXs);

  m_combo = new QComboBox(this);
  m_combo->setObjectName(prefix + QStringLiteral("OnlineBasemap"));
  m_combo->setAccessibleName(tr("GIS 底图"));
  m_combo->addItem(tr("底图：无"), ob::noneId());
  for (const auto &source : ob::sources())
    m_combo->addItem(tr("底图：%1").arg(source.title), source.id);
  lay->addWidget(m_combo);

  m_more = new QToolButton(this);
  m_more->setObjectName(prefix + QStringLiteral("BasemapMore"));
  m_more->setText(tr("更多底图"));
  m_more->setToolTip(tr("加载自定义 XYZ 瓦片、本地栅格/矢量文件或工程中已有的 GIS 图层作为底图"));
  m_more->setPopupMode(QToolButton::InstantPopup);
  auto *menu = new QMenu(m_more);
  menu->addAction(tr("自定义 XYZ 瓦片地址…"), this, &PreviewBasemapControl::promptXyz);
  menu->addAction(tr("本地栅格/矢量文件…"), this, &PreviewBasemapControl::promptFile);
  auto *layers = menu->addMenu(tr("工程图层"));
  layers->setObjectName(prefix + QStringLiteral("BasemapLayerMenu"));
  connect(layers, &QMenu::aboutToShow, this, [this, layers] { populateLayerMenu(layers); });
  menu->addSeparator();
  menu->addAction(tr("移除底图"), this, [this] { applyChoice(ob::noneId()); });
  m_more->setMenu(menu);
  lay->addWidget(m_more);

  if (!m_status) {
    auto *own = new QLabel(this);
    own->setObjectName(prefix + QStringLiteral("BasemapStatus"));
    own->setMaximumWidth(360);
    PaleoTheme::applyThemedStyleSheet(own, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    lay->addWidget(own);
    m_status = own;
  }

  if (page) {
    bm::onCanvasCrsChanged(page->mapCanvas()->canvas(), this, [this] { updateAvailability(); });
  }
  connect(m_combo, &QComboBox::currentIndexChanged, this,
          [this](int) { applyChoice(m_combo->currentData().toString()); });
  updateAvailability();
}

void PreviewBasemapControl::updateAvailability()
{
  QString reason;
  const bool canvasOk = m_page && ob::canOverlay(m_page->mapCanvas()->canvas(), &reason);
  m_unavailableReason = canvasOk ? QString() : reason;
  m_combo->setEnabled(canvasOk);
  m_combo->setToolTip(canvasOk ? tr("叠加开源免费在线底图（需联网）；工程自带离线底图时默认不叠加。"
                                    "自定义瓦片源见设置 basemap/onlineSources 或「更多底图」")
                               : reason + tr("；仍可在「更多底图」加载同坐标系的本地文件或工程图层"));
}

void PreviewBasemapControl::setStatus(const QString &text)
{
  if (!m_status) return;
  m_status->setText(text);
  m_status->setToolTip(text);
}

QString PreviewBasemapControl::statusText() const
{
  return m_status ? m_status->text() : QString();
}

QObject *PreviewBasemapControl::basemapLayer() const
{
  return m_page ? ob::findLayer(m_page->mapCanvas()->layers()) : nullptr;
}

void PreviewBasemapControl::selectInCombo(const QString &spec)
{
  const QSignalBlocker block(m_combo);
  // 自定义来源（xyz/file/layer）用一个动态条目展示，放在预置源之后。
  const int customIndex = m_combo->findData(QStringLiteral("__custom__"), Qt::UserRole + 1);
  int index = m_combo->findData(spec);
  if (index < 0 && bm::decode(spec).kind != bm::Kind::Preset && bm::decode(spec).kind != bm::Kind::None) {
    const QString text = tr("底图：%1").arg(bm::displayName(spec, m_project));
    if (customIndex >= 0) {
      m_combo->setItemText(customIndex, text);
      m_combo->setItemData(customIndex, spec);
      index = customIndex;
    } else {
      m_combo->addItem(text, spec);
      index = m_combo->count() - 1;
      m_combo->setItemData(index, QStringLiteral("__custom__"), Qt::UserRole + 1);
    }
  }
  m_combo->setCurrentIndex(index < 0 ? 0 : index);
}

bool PreviewBasemapControl::applyChoice(const QString &spec, bool remember)
{
  if (!m_page) return false;
  auto *canvas = m_page->mapCanvas()->canvas();
  if (auto *old = ob::findLayer(m_page->mapCanvas()->layers())) {
    m_page->removeMapLayer(old);
    ob::destroyLayer(old);
  }
  const bm::Choice choice = bm::decode(spec);
  QString note;
  auto *layer = bm::build(spec, canvas, m_project, m_page, &note);
  if (choice.kind != bm::Kind::None && !layer) {
    selectInCombo(ob::noneId());
    m_current = ob::noneId();
    setStatus(tr("底图未加载：%1").arg(note));
    return false;
  }
  m_current = choice.kind == bm::Kind::None ? ob::noneId() : spec;
  if (remember) bm::saveChoice(m_project, m_current);
  selectInCombo(m_current);
  if (layer) {
    m_page->addMapLayerAtBottom(layer, tr("底图：%1").arg(bm::displayName(spec, m_project)));
    if (choice.kind == bm::Kind::Preset || choice.kind == bm::Kind::Xyz) {
      QPointer<PreviewBasemapControl> self(this);
      ob::watchFailures(canvas, layer, kTileTimeoutMs, [self](const QString &message) {
        if (self) self->setStatus(message);
      });
    }
  }
  const QString attribution = paleo::mapreference::attribution(canvas);
  QStringList parts;
  if (!note.isEmpty()) parts << note;
  if (!attribution.isEmpty()) parts << attribution;
  setStatus(parts.join(QStringLiteral(" · ")));
  return true;
}

void PreviewBasemapControl::promptXyz()
{
  bool ok = false;
  const QString url = QInputDialog::getText(
      this, tr("自定义 XYZ 瓦片底图"),
      tr("瓦片地址模板（含 {x} {y} {z}，如 https://example.com/{z}/{x}/{y}.png）："),
      QLineEdit::Normal, QString(), &ok);
  if (ok && !url.trimmed().isEmpty()) applyChoice(bm::encode({bm::Kind::Xyz, url.trimmed()}));
}

void PreviewBasemapControl::promptFile()
{
  const QString path = QFileDialog::getOpenFileName(
      this, tr("选择底图文件"), QString(),
      tr("GIS 底图 (*.tif *.tiff *.img *.jpg *.png *.mbtiles *.vrt *.shp *.gpkg *.geojson *.json *.kml);;所有文件 (*)"));
  if (!path.isEmpty()) applyChoice(bm::encode({bm::Kind::File, path}));
}

void PreviewBasemapControl::populateLayerMenu(QMenu *menu)
{
  menu->clear();
  const auto candidates = bm::projectCandidateNames(m_project);
  if (candidates.isEmpty()) {
    menu->addAction(tr("（工程中没有可用的 GIS 图层）"))->setEnabled(false);
    return;
  }
  for (const auto &[id, name] : candidates) {
    const QString spec = bm::encode({bm::Kind::ProjectLayer, id});
    menu->addAction(name, this, [this, spec] { applyChoice(spec); });
  }
}

PreviewBasemapControl *attachPreviewBasemap(PreviewMapPage *page, QObject *project,
                                            const QString &prefix, QLabel *status)
{
  if (!page) return nullptr;
  auto *control = new PreviewBasemapControl(page, project, prefix, status);
  page->addToolBarWidget(control);
  const QString saved = bm::savedChoice(project);
  if (bm::decode(saved).kind != bm::Kind::None) control->applyChoice(saved, false);
  return control;
}
