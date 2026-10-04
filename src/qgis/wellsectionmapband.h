// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

class QgsMapCanvas;
class QgsRubberBand;
class QgsVectorLayer;

// qgis/ — 连井剖面在主地图的呈现面（goal/wellsection-deep 剖面-平面联动）：
//   · 剖面线高亮：井序连线画 QgsRubberBand（主色虚线，纸面语义的地图侧
//     镜像）；井集/井序变化时由壳层重喂。
//   · 反向定位闪烁：剖面点名 → well 层 fid 解析 → flashFeatureIds。
// 不解析井数据（坐标来自视图层的 wellsection::Well），不做选中联动
//（WellMapLink 的职责），只画/闪。
class WellSectionMapBand : public QObject
{
  Q_OBJECT
public:
  explicit WellSectionMapBand(QgsMapCanvas *canvas, QObject *parent = nullptr);
  ~WellSectionMapBand() override;

  // well 井位层（fid 解析 + 线位 CRS 源）；空 → 闪烁降级为 no-op。
  void setWellLayer(QgsVectorLayer *layer, const QString &idField);

  // 剖面线（井序连线，坐标 = 井位层 CRS）。空 → 隐藏。
  void setSectionPath(const QVector<QPair<double, double>> &points);

  // 反向定位：well id → 闪烁（3 × 500 ms，主色渐隐）。
  void flashWell(const QString &wellId);

private:
  QgsMapCanvas *m_canvas = nullptr;
  QPointer<QgsRubberBand> m_band;
  QPointer<QgsVectorLayer> m_layer;
  QString m_idField;
};
