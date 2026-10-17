// 层：QGIS 封装
#include "faciesmaplegend.h"
#include "../domain/faciestaxonomy.h"
#include "../domain/facieshierarchy.h"
#include <QPointer>
#include <utility>
#include <qgsproject.h>
#include <qgslayoutmanager.h>
#include <qgslayout.h>
#include <qgslayoutitemlegend.h>
#include <qgslayertree.h>
#include <qgslayertreefiltersettings.h>
#include <QIcon>
#include <QPixmap>
#include <qgslayertreemodellegendnode.h>
#include <qgsmaplayerlegend.h>
#include <qgslegendsettings.h>
#include <qgspalettedrasterrenderer.h>
#include <qgsrasterlayer.h>
#include <qgsrenderer.h>
#include <qgsvectorlayer.h>
#include <qgssymbollayerutils.h>

namespace {
// QGIS 图层树识别 parent key，但版面绘制仍逐行展开，需要给原生符号加层级缩进。
template<class NativeNode> class IndentedNode : public NativeNode {
public:
  template<class... Args> IndentedNode(int depth, Args&&... args)
      : NativeNode(std::forward<Args>(args)...), m_depth(depth) {}
  QgsLayerTreeModelLegendNode::ItemMetrics draw(const QgsLegendSettings &settings,
      QgsLayerTreeModelLegendNode::ItemContext &context) override {
    auto local = context;
    const double indent = m_depth * 2.0; // 版面单位 mm，与 QGIS 图例边距一致。
    if (settings.symbolAlignment() == Qt::AlignRight) local.columnRight -= indent;
    else local.columnLeft += indent;
    auto metrics = NativeNode::draw(settings, local);
    metrics.labelSize.rwidth() += indent;
    return metrics;
  }
private:
  int m_depth;
};
class HeadingNode final : public IndentedNode<QgsSimpleLegendNode> {
public:
  HeadingNode(QgsLayerTreeLayer *layer, const QVariantMap &row)
      : IndentedNode(row.value("depth").toInt(), layer, row.value("name").toString(), QIcon(), nullptr,
                            row.value("key").toString()),
        m_parent(row.value("parent_key").toString()) {}
  QVariant data(int role) const override {
    if (role == int(CustomRole::ParentRuleKey)) return m_parent;
    return QgsSimpleLegendNode::data(role);
  }
private:
  QString m_parent;
};

class HierarchyLegend final : public QgsMapLayerLegend {
public:
  explicit HierarchyLegend(QgsMapLayer *layer) : QgsMapLayerLegend(layer), m_layer(layer) {
    connect(layer, &QgsMapLayer::rendererChanged, this, &QgsMapLayerLegend::itemsChanged);
  }
  QList<QgsLayerTreeModelLegendNode *> createLayerTreeModelLegendNodes(QgsLayerTreeLayer *node) override {
    QList<QgsLayerTreeModelLegendNode *> result;
    if (!m_layer) return result;
    const auto entries = m_layer->customProperty("paleo/faciesDisplayLegend").toList();
    const auto tree = FaciesHierarchy::legendTree(entries);
    auto *vector = qobject_cast<QgsVectorLayer *>(m_layer.data());
    const auto symbols = vector && vector->renderer() ? vector->renderer()->legendSymbolItems() : QgsLegendSymbolList{};
    const auto *raster = qobject_cast<QgsRasterLayer *>(m_layer.data());
    const auto *palette = raster ? dynamic_cast<QgsPalettedRasterRenderer *>(raster->renderer()) : nullptr;
    const auto classes = palette ? palette->classes() : QgsPalettedRasterRenderer::ClassData{};
    for (const auto &value : tree) {
      const auto row = value.toMap();
      if (row.value("heading").toBool()) {
        result << new HeadingNode(node, row);
        continue;
      }
      if (vector) {
        // 保留实际渲染器 rule key：QGIS 的按图过滤、显隐和布局符号查找依赖它。
        for (int i = 0; i < entries.size() && i < symbols.size(); ++i) {
          if (entries[i].toMap().value("key") != row.value("key")) continue;
          const auto &source = symbols[i];
          const QgsLegendSymbolItem item(source.symbol(), row.value("name").toString(), source.ruleKey(),
              source.isCheckable(), source.scaleMinDenom(), source.scaleMaxDenom(), row.value("depth").toInt(),
              row.value("parent_key").toString());
          result << new IndentedNode<QgsSymbolLegendNode>(row.value("depth").toInt(), node, item);
          break;
        }
      } else if (palette) {
        QString ruleKey;
        const auto codes = row.value("codes").toList();
        for (int i = 0; i < classes.size(); ++i)
          if (codes.contains(int(classes[i].value))) { ruleKey = QString::number(i); break; }
        result << new IndentedNode<QgsRasterSymbolLegendNode>(row.value("depth").toInt(), node, QColor(row.value("color").toString()),
            row.value("name").toString(), nullptr, false, ruleKey, row.value("parent_key").toString());
      }
    }
    QgsMapLayerLegendUtils::applyLayerNodeProperties(node, result);
    return result;
  }
private:
  QPointer<QgsMapLayer> m_layer;
};
}
void FaciesMapLegend::install(QgsMapLayer *layer) {
  if (!layer) return;
  layer->setLegend(new HierarchyLegend(layer));
  // QGIS 的 legendChanged 只重建节点，版面仍缓存旧渲染器的按图命中结果。
  // 显式刷新包含此层的版面图例，使层级和改类后的过滤同步重新计算。
  if (auto *project = layer->project())
    for (auto *master : project->layoutManager()->layouts())
      if (auto *layout = dynamic_cast<QgsLayout *>(master)) {
        QList<QgsLayoutItemLegend *> legends;
        layout->layoutItems(legends);
        for (auto *legend : legends)
          if (auto *root = legend->model()->rootGroup(); root && root->findLayer(layer->id())) {
            if (const auto *filter = legend->model()->filterSettings()) {
              const QgsLayerTreeFilterSettings settings(*filter);
              legend->model()->setFilterSettings(&settings);
            }
            legend->refresh();
          }
      }
}
QVariantList FaciesMapLegend::symbolEntries(QgsMapLayer *layer, const QVariantList &entries) {
  QVariantList migrated;
  FaciesTaxonomy::upgrade(entries, &migrated);  // 旧冻结分类迁进三级体系
  const auto effective = migrated.isEmpty() && !entries.isEmpty() ? entries : migrated;
  const auto *vector = qobject_cast<QgsVectorLayer *>(layer);
  const auto symbols = vector && vector->renderer() ? vector->renderer()->legendSymbolItems() : QgsLegendSymbolList{};
  QVariantList result;
  for (int i = 0; i < effective.size(); ++i) {
    auto row = effective[i].toMap();
    QIcon icon;
    if (i < symbols.size()) icon = QgsSymbolLayerUtils::symbolPreviewIcon(symbols[i].symbol(), QSize(32, 16));
    else {
      QPixmap swatch(32, 16);
      swatch.fill(QColor(row.value("color").toString()));
      icon = QIcon(swatch);
    }
    row.insert("legend_icon", icon);
    result << row;
  }
  return result;
}
