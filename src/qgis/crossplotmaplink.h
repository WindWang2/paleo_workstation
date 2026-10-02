// 层：QGIS 封装
#pragma once
#include "domain/crossplotsamples.h"
#include <QObject>
#include <QPointer>
class QgsMapCanvas;
class QgsRubberBand;
namespace paleo::crossplot {
class CrossplotMapLink : public QObject {
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
