// 层：QGIS 封装
#include "wellsectionmapband.h"

#include <qgscoordinatetransform.h>
#include <qgsexpression.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfields.h>
#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsrubberband.h>
#include <qgsvectorlayer.h>

#include <QColor>
#include <QtGlobal>

WellSectionMapBand::WellSectionMapBand(QgsMapCanvas *canvas,
                                       QObject *parent)
    : QObject(parent), m_canvas(canvas)
{
}

WellSectionMapBand::~WellSectionMapBand()
{
  if (m_band)
    m_band->deleteLater();
}

void WellSectionMapBand::setWellLayer(QgsVectorLayer *layer,
                                      const QString &idField)
{
  m_layer = layer;
  m_idField = idField;
}

void WellSectionMapBand::setSectionPath(
    const QVector<QPair<double, double>> &points)
{
  if (!m_canvas)
    return;
  if (points.size() < 2)
  {
    if (m_band)
      m_band->hide();
    return;
  }
  if (!m_band)
  {
    m_band = new QgsRubberBand(m_canvas, Qgis::GeometryType::Line);
    // 主 token 色（chrome 侧联动元素，非纸面图件）。
    m_band->setColor(QColor(QStringLiteral("#1B73D0")));
    m_band->setWidth(2);
    m_band->setLineStyle(Qt::DashLine);
  }
  // 井位层 CRS → 地图目标 CRS（无层时按已是地图坐标处理）。
  const QgsCoordinateReferenceSystem source =
      m_layer ? m_layer->crs() : QgsCoordinateReferenceSystem();
  QgsCoordinateTransform transform;
  if (m_layer && source.isValid())
    transform = QgsCoordinateTransform(
        source, m_canvas->mapSettings().destinationCrs(),
        QgsProject::instance());
  m_band->reset(Qgis::GeometryType::Line);
  for (const auto &pt : points)
  {
    QgsPointXY p(pt.first, pt.second);
    if (transform.isValid())
    {
      try
      {
        p = transform.transform(p);
      }
      catch (const QgsCsException &)
      {
        // 单点变换失败 → 跳过该点（线断比整带消失好）。
        continue;
      }
    }
    m_band->addPoint(p);
  }
  m_band->show();
}

void WellSectionMapBand::flashWell(const QString &wellId)
{
  if (!m_canvas || !m_layer || wellId.isEmpty())
    return;
  const int idx = m_layer->fields().indexOf(m_idField);
  if (idx < 0)
    return;
  QgsFeatureRequest req;
  req.setFilterExpression(QStringLiteral("%1 = '%2'").arg(
      QgsExpression::quotedColumnRef(m_idField),
      QgsExpression::quotedValue(wellId)));
  req.setSubsetOfAttributes(QgsAttributeList{idx});
  req.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
  QgsFeature f;
  QgsFeatureIterator it = m_layer->getFeatures(req);
  QgsFeatureIds fids;
  while (it.nextFeature(f))
    fids << f.id();
  if (fids.isEmpty())
    return;
  const QColor primary(QStringLiteral("#1B73D0"));
  m_canvas->flashFeatureIds(m_layer, fids, primary,
                            QColor(primary.red(), primary.green(), primary.blue(), 0),
                            3, 500);
}
