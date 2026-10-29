// 层：QGIS 封装
#pragma once
#include "domain/crossplotsamples.h"
#include <QObject>
#include <QPointer>
class QgsMapCanvas;
class QgsRubberBand;
namespace paleo::crossplot {
// #70：缺 Q_OBJECT 时 tr() 落入 QObject 翻译上下文，lupdate 无法把面向用户
// 的错误文案归到本类名下（zh_CN 下仍是英文）。补宏后需重跑 lupdate 归位。
class CrossplotMapLink : public QObject {
  Q_OBJECT
public:
  explicit CrossplotMapLink(QgsMapCanvas *, QObject *parent = nullptr);
  ~CrossplotMapLink() override;
  bool locate(const SampleSet &, int sample, QString *error);
  bool highlight(const SampleSet &, const QVector<int> &, QString *error);
  void clear();

private:
  bool coordinate(const SampleSet &, int, double *, double *, QString *) const;
  QPointer<QgsMapCanvas> m_canvas;
  QPointer<QgsRubberBand> m_band;
};
} // namespace paleo::crossplot
