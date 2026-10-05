// 层：QGIS 封装
#pragma once
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

class QgsMapLayer;
class QgsProject;

// qgis/semanticlegend.h — 语义图例自动生成（方向 31 批 3）。
// 按图层渲染器类目自动出图例项（花纹/符号 swatch + 语义词面），供
// 方向 25 布局图例元素与图例面板复用；图层/renderer 变化发 legendChanged
// 联动刷新。swatch 与图层渲染同源（renderer 类目符号的预览渲染，QgsSvgCache
// 同一管线），保证「图例所见 = 图面所得」。
//
// 方向 49 定性：有意预留，非死代码——宿主接线递延至方向 24/25 的页面 UX
// 决策（方向 31 账本批 4「主窗口 dock/页面接线未做」），当前消费面仅
// tst_semanticlegend。回收条件：方向 24/25 图例面板/布局图例收口后若仍无
// 生产消费，删除本件 + tst_semanticlegend + CMakeLists 两处接线
//（paleo_qgis 源列表与 add_paleo_test）。
struct SemanticLegendItem
{
  QString label;      // 类目词面（词表 title/renderer label）
  QImage swatch;      // 类目符号预览（QgsSymbol::bigSymbolPreviewImage）
  QString semanticId; // 渲染器类目原值（GeoPatterns id 或数据值；single 层空）
};

struct SemanticLegendSection
{
  QString layerId;
  QString title; // 图层名（图例节标题）
  QVector<SemanticLegendItem> items;
};

class SemanticLegendBuilder : public QObject
{
  Q_OBJECT
public:
  // 只收矢量层（栅格/网面无符号类目语义）；节序 = 图层树序（与画布 z 序
  // 一致的图例惯例）。project 须非空；builder 不持工程所有权。
  explicit SemanticLegendBuilder(QgsProject *project, QObject *parent = nullptr);
  ~SemanticLegendBuilder() override;

  QVector<SemanticLegendSection> build() const;

signals:
  // 图层增删、renderer 替换、图层名变化后发出（UI/布局图例重拉 build()）。
  void legendChanged();

private:
  void watchLayer(QgsMapLayer *layer);
  void unwatchLayer(const QString &layerId);

  QgsProject *m_project = nullptr;
  QHash<QString, QMetaObject::Connection> m_rendererConns;
};
