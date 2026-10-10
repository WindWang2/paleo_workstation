// 层：视图
#include "datapreviewtabs.h"
#include "datapreviewtabs_internal.h"

#include <QAction>

// 共享辅助来自内部头——与 datapreviewtabs.cpp 用同一 using 引入（族内先例：
// datapreviewtabworkbook/tabxml 同款）。
using namespace paleo::datapreview_detail;

// ---- D2.10 同目录组图：叠一层同目录可地图化资产（geojson/带配准图片/
// 层位栅格）。不支持的类型如实跳过，不造假层。----
void DataPreviewTabs::addSiblingOverlayButton(DataCatalog *catalog, const QString &sourcePath,
                                               const QString &assetId, PreviewMapPage *page, QWidget *owner)
{
  const auto siblings = PreviewMapStates::siblingMappableAssets(
      catalog, sourcePath, [this](const CatalogVersion &v) { return m_doc->absolutePathForVersion(v); });
  QVector<QPair<QString, QString>> others;
  for (const auto &sib : siblings)
    if (sib.first != assetId)
      others.append(sib);
  if (others.isEmpty())
    return;
  auto *overlayBtn = new QToolButton(page);
  overlayBtn->setObjectName(QStringLiteral("siblingOverlayButton"));
  overlayBtn->setText(tr("同目录叠加"));
  overlayBtn->setToolTip(tr("把同目录下的相图/配准图片/层位栅格叠加到本预览"));
  overlayBtn->setPopupMode(QToolButton::InstantPopup);
  auto *menu = new QMenu(overlayBtn);
  for (const auto &sib : others)
  {
    QAction *act = menu->addAction(sib.second);
    QObject::connect(act, &QAction::triggered, owner, [this, page, sib, owner]() {
      addSiblingOverlayLayer(page, sib.first, sib.second, owner);
    });
  }
  overlayBtn->setMenu(menu);
  page->addToolBarWidget(overlayBtn);
}

void DataPreviewTabs::addSiblingOverlayLayer(PreviewMapPage *page, const QString &sibAssetId,
                                             const QString &sibName, QWidget *owner)
{
  if (!page || !m_doc || sibAssetId.isEmpty())
    return;
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat->assetById(sibAssetId);
  if (asset.id.isEmpty())
    return;
  const CatalogVersion v = cat->currentVersion(sibAssetId);
  const QString abs = m_doc->absolutePathForVersion(v);
  if (abs.isEmpty() || !QFile::exists(abs))
    return;

  if (asset.type == QLatin1String("geojson") ||
      (asset.type == QLatin1String("boundary") &&
       abs.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive)))
  {
    auto *vl = new QgsVectorLayer(abs, sibName, QStringLiteral("ogr"));
    vl->setParent(owner);
    if (!vl->isValid())
    {
      vl->deleteLater();
      return;
    }
    QString field;
    for (const QgsField &f : vl->fields())
    {
      const QString n = f.name();
      if (n == QLatin1String("相") || n.contains(QLatin1String("相")) ||
          n.compare(QLatin1String("facies"), Qt::CaseInsensitive) == 0)
      {
        field = n;
        break;
      }
    }
    if (!field.isEmpty())
      applyFaciesRendererToLayer(vl, field);
    page->addMapLayer(vl, sibName, abs);
    return;
  }
  if (asset.type == QLatin1String("image_reference"))
  {
    if (PreviewMapStates::detectWorldFile(abs).isEmpty())
      return; // 未配准图片不进地图（D2.7 语义）
    auto *rl = new QgsRasterLayer(abs, sibName, QStringLiteral("gdal"));
    rl->setParent(owner);
    if (!rl->isValid() || rl->extent().isEmpty())
    {
      rl->deleteLater();
      return;
    }
    page->addMapLayer(rl, sibName, abs);
    return;
  }
  if (asset.type == QLatin1String("horizon"))
  {
    CatalogVersion best;
    for (const CatalogVersion &cv : cat->versionsForAsset(sibAssetId))
      if (cv.stage == QLatin1String("DERIVED") && cv.versionNumber >= best.versionNumber)
        best = cv;
    if (best.id.isEmpty())
      return;
    const QString tif = m_doc->absolutePathForVersion(best);
    auto *rl = new QgsRasterLayer(tif, sibName, QStringLiteral("gdal"));
    rl->setParent(owner);
    if (!rl->isValid() || rl->extent().isEmpty())
    {
      rl->deleteLater();
      return;
    }
    const auto sum = PreviewRasterAnalysis::summarize(rl);
    if (sum.valid)
      PreviewRasterAnalysis::applyPseudoColorRenderer(
          rl, 1, sum.min, sum.max,
          *PreviewRasterAnalysis::rampPreset(QStringLiteral("terrain")), false,
          PreviewRasterAnalysis::Classification::Continuous);
    page->addMapLayer(rl, sibName, tif);
    return;
  }
}
