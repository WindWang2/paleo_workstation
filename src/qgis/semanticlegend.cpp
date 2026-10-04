// 层：QGIS 封装
#include "semanticlegend.h"

#include <QImage>

#include <qgscategorizedsymbolrenderer.h>
#include <qgslayertree.h>
#include <qgslegendsymbolitem.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrenderer.h>
#include <qgssymbol.h>
#include <qgsvectorlayer.h>

// qgis/semanticlegend.cpp — build() 按图层树显示序（layerOrder，图例与
// 画布 z 序同源）遍历矢量层收渲染器类目：分类渲染器逐类目（词面 +
// 数据值 semanticId + 符号预览），其余渲染器走 legendSymbolItems() 通用
// 回退（single 类目无词面 → 图层名）。联动：layersAdded/layersRemoved +
// 每层 rendererChanged/nameChanged → legendChanged（图例无增量状态，
// 收到信号重拉 build() 即一致）。

namespace
{
  QImage swatchFor(QgsSymbol *symbol)
  {
    if (!symbol)
      return QImage();
    return symbol->bigSymbolPreviewImage(
        nullptr, Qgis::SymbolPreviewFlag::FlagIncludeCrosshairsForMarkerSymbols);
  }
} // namespace

SemanticLegendBuilder::SemanticLegendBuilder(QgsProject *project, QObject *parent)
  : QObject(parent), m_project(project)
{
  if (!m_project)
    return;
  connect(m_project, &QgsProject::layersAdded, this, [this](const QList<QgsMapLayer *> &layers) {
    for (QgsMapLayer *layer : layers)
      watchLayer(layer);
    emit legendChanged();
  });
  connect(m_project, &QgsProject::layersRemoved, this, [this](const QStringList &ids) {
    for (const QString &id : ids)
      unwatchLayer(id);
    emit legendChanged();
  });
  const auto projectLayers = m_project->mapLayers();
  for (auto it = projectLayers.cbegin(); it != projectLayers.cend(); ++it)
    watchLayer(it.value());
}

SemanticLegendBuilder::~SemanticLegendBuilder()
{
  for (auto it = m_rendererConns.cbegin(); it != m_rendererConns.cend(); ++it)
    disconnect(it.value());
}

void SemanticLegendBuilder::watchLayer(QgsMapLayer *layer)
{
  if (!layer || m_rendererConns.contains(layer->id()))
    return;
  const QMetaObject::Connection conn =
      connect(layer, &QgsMapLayer::rendererChanged, this,
              &SemanticLegendBuilder::legendChanged);
  m_rendererConns.insert(layer->id(), conn);
  // 图层名即图例节标题：改名也联动（UniqueConnection 防重复挂）。
  connect(layer, &QgsMapLayer::nameChanged, this, &SemanticLegendBuilder::legendChanged,
          Qt::UniqueConnection);
}

void SemanticLegendBuilder::unwatchLayer(const QString &layerId)
{
  const auto it = m_rendererConns.find(layerId);
  if (it != m_rendererConns.end())
  {
    disconnect(it.value());
    m_rendererConns.erase(it);
  }
}

QVector<SemanticLegendSection> SemanticLegendBuilder::build() const
{
  QVector<SemanticLegendSection> sections;
  if (!m_project)
    return sections;
  // 树显示序 = 图例序（含勾选层；离树/未勾选不收——图例跟画布可见性走）。
  const QList<QgsMapLayer *> ordered = m_project->layerTreeRoot()->layerOrder();
  for (QgsMapLayer *layer : ordered)
  {
    auto *vector = qobject_cast<QgsVectorLayer *>(layer);
    if (!vector || !vector->renderer())
      continue;
    SemanticLegendSection section;
    section.layerId = vector->id();
    section.title = vector->name();

    if (auto *cat = dynamic_cast<QgsCategorizedSymbolRenderer *>(vector->renderer()))
    {
      // 分类渲染器：逐类目，semanticId = 类目数据值（GeoPatterns 词表 id
      // 或字段原值——词面归一归调用方，图例不吞数据）。
      const QgsCategoryList categories = cat->categories();
      for (const QgsRendererCategory &c : categories)
      {
        SemanticLegendItem out;
        out.label = c.label().isEmpty() ? section.title : c.label();
        out.swatch = swatchFor(c.symbol());
        out.semanticId = c.value().isValid() ? c.value().toString() : QString();
        section.items.append(out);
      }
    }
    else
    {
      const QgsLegendSymbolList legendItems = vector->renderer()->legendSymbolItems();
      for (const QgsLegendSymbolItem &item : legendItems)
      {
        SemanticLegendItem out;
        out.label = item.label().isEmpty() ? section.title : item.label();
        out.swatch = swatchFor(item.symbol());
        section.items.append(out);
      }
    }
    if (!section.items.isEmpty())
      sections.append(section);
  }
  return sections;
}
