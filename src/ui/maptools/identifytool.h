// 层：视图
#pragma once

#include <QPoint>

#include "../../qgis/maptoolviewapi.h"

class QKeyEvent;

class IdentifyPopup;
class PreviewIdentifyCore;
class QTimer;

// 主画布识别浏览：指针落到要素上时弹出悬浮信息，多个命中可切换。
// QGIS 类型只经 src/qgis/maptoolviewapi.h 门面（别名）进入本头——
// 视图层 TU 不直连外部 qgs* 头（#472 双闸：qgs-audit/qgis-spread）。
class PaleoIdentifyTool : public PaleoMapTool
{
  Q_OBJECT
public:
  explicit PaleoIdentifyTool(PaleoMapCanvas *canvas);
  ~PaleoIdentifyTool() override;

  void canvasMoveEvent(PaleoMapMouseEvent *event) override;
  void canvasPressEvent(PaleoMapMouseEvent *event) override;
  void canvasReleaseEvent(PaleoMapMouseEvent *event) override;
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
