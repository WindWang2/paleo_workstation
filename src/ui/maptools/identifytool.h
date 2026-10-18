// 层：视图
#pragma once

#include <QPoint>
#include <qgsmaptool.h>

class IdentifyPopup;
class PreviewIdentifyCore;
class QTimer;

// 主画布识别浏览：指针落到要素上时弹出悬浮信息，多个命中可切换。
class PaleoIdentifyTool : public QgsMapTool
{
  Q_OBJECT
public:
  explicit PaleoIdentifyTool(QgsMapCanvas *canvas);
  ~PaleoIdentifyTool() override;

  void canvasMoveEvent(QgsMapMouseEvent *event) override;
  void canvasPressEvent(QgsMapMouseEvent *event) override;
  void canvasReleaseEvent(QgsMapMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void deactivate() override;

private:
  void refreshAt(const QPoint &pixel, bool pin);
  void hidePopup();

  IdentifyPopup *m_popup = nullptr;
  PreviewIdentifyCore *m_core = nullptr;
  QTimer *m_hover = nullptr;
  QPoint m_pending;
  bool m_pinned = false;
  bool m_overPopup = false;
};
