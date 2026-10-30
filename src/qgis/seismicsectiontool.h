// 层：QGIS 封装
#pragma once

#include <qgsmaptool.h>
#include <qgspointxy.h>
#include <qgsrubberband.h>
#include <QStringList>
#include <QVector>

#include "../domain/wellsection.h" // wellsection::Well（最近井吸附纯算法在 domain）

class QgsVertexMarker;

// Interactive map tool for capturing an arbitrary section polyline directly on the QgsMapCanvas.
// Left click adds points; right click finishes capture and emits sectionPathCaptured.
// 井位吸附（连井剖面吸附成线）：setSnapWells 注入有效井集后，鼠标移动时
// 容差内最近井吸附预览（顶点标记 + tooltip 井名）；吸附态落点取井口精确
// 坐标并把 well id 按序收集（连续同井忽略），随完成信号一并发出——壳据
// ≥2 口吸附井按该井序生成过井剖面，否则按纯任意线提取。
class SeismicSectionTool : public QgsMapTool
{
  Q_OBJECT

public:
  explicit SeismicSectionTool(QgsMapCanvas *canvas);
  ~SeismicSectionTool() override;

  void activate() override;
  void deactivate() override;

  // 吸附井集（有效坐标井；空集 = 关闭吸附，行为同纯任意线）。
  void setSnapWells(const QVector<wellsection::Well> &wells,const QString& sourceCrs = {});

signals:
  // 完成：折线顶点 + 吸附井的有序 id 列表（<2 口时壳按纯任意线处理）。
  void sectionPathCaptured(const QVector<QgsPointXY> &points, const QStringList &wellIds);
  // Esc 取消：壳据此把为绘制而隐藏的设置对话框唤回。
  void captureCancelled();

protected:
  void canvasPressEvent(QgsMapMouseEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
  void canvasMoveEvent(QgsMapMouseEvent *e) override;

private:
  void redraw(const QgsPointXY *hover = nullptr);
  void clearSnapFeedback();
  double snapToleranceMapUnits() const;
  QVector<QgsPointXY> m_points;
  QStringList m_vertexWellIds; // 与 m_points 等长；自由顶点为空串
  QVector<wellsection::Well> m_snapWells;
  QgsRubberBand *m_rubberBand = nullptr;
  QgsVertexMarker *m_snapMarker = nullptr;
};
