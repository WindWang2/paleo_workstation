// 层：QGIS 封装
#include "geologicalmaplayout.h"
#include <QDate>
#include <qgscoordinatetransform.h>
#include <qgsmaplayerstyle.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutitempage.h>
#include <qgslayertree.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>
#include <memory>

namespace PaleoGeologicalMapLayout {
bool isFaciesPolygon(QgsMapLayer *layer) {
  const auto *vector = qobject_cast<QgsVectorLayer *>(layer);
  return vector && vector->geometryType() == Qgis::GeometryType::Polygon &&
      (vector->fields().indexOf("microfacies") >= 0 || vector->fields().indexOf("facies_code") >= 0 ||
       (vector->fields().indexOf("cluster_id") >= 0 && vector->fields().indexOf("patch_id") >= 0));
}

static QMap<QString, QString> overviewStyles(const QList<QgsMapLayer *> &layers) {
  QMap<QString, QString> styles;
  for (auto *layer : layers) {
    if (!isFaciesPolygon(layer) || !qobject_cast<QgsVectorLayer *>(layer)->labelsEnabled()) continue;
    QgsMapLayerStyle style;
    style.readFromLayer(layer);
    QDomDocument doc;
    if (!doc.setContent(style.xmlData())) continue;
    // Override only the print map. Keep the live project's labels, symbols, and other layers intact.
    doc.documentElement().setAttribute("labelsEnabled", "0");
    styles.insert(layer->id(), doc.toString());
  }
  return styles;
}
bool validateBounds(QgsPrintLayout *layout, QString *error) {
  if (!layout) return false;
  for (const auto &id : QStringList{"map", "scalebar", "legend", "northArrow", "title", "subtitle", "signature", "crsCaption", "inset"}) {
    auto *item = layout->itemById(id);
    if (!item || item->page() < 0) continue;
    auto *page = layout->pageCollection()->page(item->page());
    if (!page) continue;
    const auto paper = page->mapRectToScene(page->rect()).adjusted(-0.5, -0.5, 0.5, 0.5);
    if (!paper.contains(item->mapRectToScene(item->rect()))) {
      if (error) *error = QObject::tr("图件元素 %1 超出纸面，请选择更大纸张、调整方向或在设计器中调整版面").arg(id == "legend" ? QObject::tr("图例") : id);
      return false;
    }
  }
  auto *legend = layout->itemById("legend"), *inset = layout->itemById("inset");
  if (legend && inset && legend->mapRectToScene(legend->rect()).intersects(inset->mapRectToScene(inset->rect()))) {
    if (error) *error = QObject::tr("图例与位置插图重叠，请选择更大纸张或在设计器中调整版面");
    return false;
  }
  return true;
}
QgsPrintLayout *build(QgsProject *project, const QList<QgsMapLayer *> &layers,
    const QgsRectangle &extent, const Settings &settings, QString *error) {
  const auto fail = [error](const QString &text) -> QgsPrintLayout * { if (error) *error = text; return nullptr; };
  if (!project || layers.isEmpty() || extent.isEmpty()) return fail(QObject::tr("当前没有可出图的地图图层或范围"));
  for (auto *layer : layers) {
    if (!layer || !layer->isValid()) return fail(QObject::tr("出图图层缺失或数据源不可用"));
    if (auto *vector = qobject_cast<QgsVectorLayer *>(layer); vector && vector->isModified())
      return fail(QObject::tr("图层 %1 有未保存编辑，请先保存版本").arg(vector->name()));
  }
  const auto canvasCrs = project->crs();
  auto printCrs = canvasCrs;
  auto printExtent = extent;
  if (settings.usePrimaryLayerCrs) {
    auto *primary = settings.primaryLayerId.isEmpty() ? layers.first() : project->mapLayer(settings.primaryLayerId);
    if (!primary || !layers.contains(primary) || !primary->crs().isValid() || !canvasCrs.isValid())
      return fail(QObject::tr("无法确定主图层或地图的坐标系，请使用当前地图坐标系。"));
    printCrs = primary->crs();
    if (printCrs != canvasCrs) {
      try {
        QgsCoordinateTransform transform(canvasCrs, printCrs, project->transformContext());
        printExtent = transform.transformBoundingBox(extent);
      } catch (const QgsCsException &e) {
        return fail(QObject::tr("无法将当前范围转换至主图层坐标系：%1").arg(e.what()));
      }
      if (printExtent.isEmpty() || !printExtent.isFinite())
        return fail(QObject::tr("当前范围在主图层坐标系中无效。"));
    }
  }
  auto layout = std::make_unique<QgsPrintLayout>(project);
  layout->initializeDefaults();
  if (!PaleoStandardElements::populateFigureLayout(layout.get(), settings.kind, settings.page, layers,
                                                    printExtent, printCrs, error)) return nullptr;
  layout->setName(settings.horizon + "_map");
  layout->setCustomProperty("paleo/export_horizon", settings.horizon);
  layout->setCustomProperty("paleo/export_version", settings.version);
  layout->setCustomProperty("paleo/export_kind", PaleoStandardElements::figureKindKey(settings.kind));
  layout->setCustomProperty("paleo/validation", "unverified");
  if (auto *title = qobject_cast<QgsLayoutItemLabel *>(layout->itemById("title")))
    title->setText(settings.title.isEmpty() ? settings.horizon + " " + PaleoStandardElements::figureKindTitle(settings.kind) : settings.title);
  if (auto *subtitle = qobject_cast<QgsLayoutItemLabel *>(layout->itemById("subtitle")))
    subtitle->setText(settings.subtitle.isEmpty() && settings.horizon.isEmpty() ? QObject::tr("当前地图数据快照 · 验证状态见成果记录") : settings.subtitle.isEmpty() ? QObject::tr("层位：%1 · %2 · 验证状态见成果记录")
        .arg(settings.horizon, settings.version > 0 ? QObject::tr("当前数据快照 · 最近地图版本 v%1").arg(settings.version) : QObject::tr("当前已保存数据快照")) : settings.subtitle);
  if (auto *signature = qobject_cast<QgsLayoutItemLabel *>(layout->itemById("signature"))) {
    const double width = layout->pageCollection()->page(0)->pageSize().width();
    signature->attemptResize(QgsLayoutSize(width - 20, 6));
    signature->attemptMove(QgsLayoutPoint(10, layout->pageCollection()->page(0)->pageSize().height() - 9));
    signature->setText(QObject::tr("编制：%1    审核：%2    日期：%3")
        .arg(settings.author.isEmpty() ? "________" : settings.author,
             settings.reviewer.isEmpty() ? "________" : settings.reviewer, QDate::currentDate().toString(Qt::ISODate)));
  }
  auto *map = qobject_cast<QgsLayoutItemMap *>(layout->itemById("map"));
  if (map) { map->setKeepLayerSet(true); map->setLayers(layers); map->setMapRotation(settings.rotation); if (settings.scale > 0) map->setScale(settings.scale); }
  if (!settings.showPatchLabels) {
    const auto styles = overviewStyles(layers);
    if (!styles.isEmpty()) for (auto *item : layout->items()) {
      if (auto *frame = dynamic_cast<QgsLayoutItemMap *>(item)) {
        frame->setLayerStyleOverrides(styles);
        frame->setKeepLayerStyles(true);
      }
    }
  }
  layout->setCustomProperty("paleo/print_patch_labels", settings.showPatchLabels);
  if (map) layout->setReferenceMap(map);
  const bool local = !printCrs.isValid() || printCrs.type() == Qgis::CrsType::Engineering || printCrs.type() == Qgis::CrsType::Unknown;
  if (auto *arrow = qobject_cast<QgsLayoutItemPicture *>(layout->itemById("northArrow")))
    arrow->setNorthMode(local ? QgsLayoutItemPicture::GridNorth : QgsLayoutItemPicture::TrueNorth);
  auto *caption = new QgsLayoutItemLabel(layout.get()); caption->setId("crsCaption");
  caption->setTextFormat(PaleoStandardElements::figureTextFormat(8));
  caption->setText(local ? QObject::tr("工程坐标 · 未配准 · 箭头表示工程网格北") :
      QObject::tr("坐标系：%1 · %2").arg(printCrs.authid().isEmpty() ? QObject::tr("自定义 CRS") : printCrs.authid(), printCrs.description()));
  layout->addLayoutItem(caption);
  const auto size = layout->pageCollection()->page(0)->pageSize();
  caption->attemptMove(QgsLayoutPoint(10, size.height() - 16));
  caption->attemptResize(QgsLayoutSize(size.width() - 20, 6));
  return layout.release();
}
std::unique_ptr<PaleoLayoutExport::LayoutHandle> buildHandle(QgsProject *project,
    const QList<QgsMapLayer *> &layers, const QgsRectangle &extent, const Settings &settings,
    QString *error) {
  std::unique_ptr<QgsPrintLayout> built(build(project, layers, extent, settings, error));
  if (!built) return nullptr;
  return std::make_unique<PaleoLayoutExport::LayoutHandle>(std::move(built));
}
QVariantMap describe(QgsPrintLayout *layout) {
  QVariantList layers;
  if (!layout) return {};
  for (auto *item : layout->items()) if (auto *map = dynamic_cast<QgsLayoutItemMap *>(item))
    for (auto *layer : map->layers()) if (layer) layers.append(QVariantMap{
        {"qgis_layer_id", layer->id()}, {"layer_id", layer->customProperty("paleoLayerId")},
        {"version_id", layer->customProperty("paleoVersionId")}, {"title", layer->name()},
        {"crs_wkt", layer->crs().toWkt()}, {"source", layer->source()}});
  auto *map = qobject_cast<QgsLayoutItemMap *>(layout->itemById("map"));
  const auto *title = qobject_cast<QgsLayoutItemLabel *>(layout->itemById("title"));
  return {{"title", title ? title->text() : QString()}, {"horizon", layout->customProperty("paleo/export_horizon")},
      {"map_crs", map ? map->crs().authid() : QString()}, {"map_crs_wkt", map ? map->crs().toWkt() : QString()},
      {"patch_labels", layout->customProperty("paleo/print_patch_labels", true)},
      {"version", layout->customProperty("paleo/export_version")},
      {"kind", layout->customProperty("paleo/export_kind")},
      {"validation", layout->customProperty("paleo/validation", "unverified")}, {"layers", layers}};
}
}
