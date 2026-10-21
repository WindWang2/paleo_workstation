// 层：QGIS 封装
#pragma once

#include <QPair>
#include <QPoint>
#include <QString>
#include <QVector>

class QgsMapCanvas;
class QgsMapTool;

// 识别命中的一行展示。工具把图层要素收成这个结构，弹层不依赖 QGIS 类型。
struct IdentifyHit
{
  QString title;
  QString subtitle;
  QVector<QPair<QString, QString>> fields;
};

// 识别结果的展示端（视图层实现，例如 IdentifyPopup）。QGIS 工具只经此接口
// 驱动展示，视图层不必直连 qgs* 头（qgs-audit / qgis-spread 闸）。
class IdentifyPresenter
{
public:
  virtual ~IdentifyPresenter() = default;
  virtual void presentHits(const QVector<IdentifyHit> &hits, const QPoint &anchor) = 0;
  virtual void dismissHits() = 0;
  virtual void stepHit(int delta) = 0;
  virtual bool hitsShown() const = 0;
  virtual bool pointerOverHits() const = 0;
};

// 主画布识别浏览：指针落到要素上时弹出悬浮信息，多个命中可切换。
// presenter 由调用方持有（通常以画布为父对象），须比工具活得久或同寿。
QgsMapTool *createIdentifyTool(QgsMapCanvas *canvas, IdentifyPresenter *presenter);
