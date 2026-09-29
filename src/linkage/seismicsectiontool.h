// 层：功能
#pragma once

#include <qgsmaptool.h>
#include <qgspointxy.h>
#include <qgsrubberband.h>
#include <QVector>

// Interactive map tool for capturing an arbitrary section polyline directly on the QgsMapCanvas.
// Left click adds points; right click finishes capture and emits sectionPathCaptured.
class SeismicSectionTool : public QgsMapTool
{
  Q_OBJECT

public:
  explicit SeismicSectionTool(QgsMapCanvas *canvas);
  ~SeismicSectionTool() override;

  void activate() override;
  void deactivate() override;

signals:
  void sectionPathCaptured(const QVector<QgsPointXY> &points);

protected:
  void canvasPressEvent(QgsMapMouseEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
  void canvasMoveEvent(QgsMapMouseEvent *e) override;

private:
  void redraw(const QgsPointXY *hover = nullptr);
  QVector<QgsPointXY> m_points;
  QgsRubberBand *m_rubberBand = nullptr;
};
