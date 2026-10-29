// 层：QGIS 封装
#include "coordinatemaptool.h"

#include <QKeyEvent>
#include <memory>
#include <qgsmapcanvas.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolemitpoint.h>
#include <qgspointlocator.h>
#include <qgssnapindicator.h>

class PaleoNativeCoordinateMapTool final : public QgsMapToolEmitPoint
{
  Q_OBJECT
public:
  explicit PaleoNativeCoordinateMapTool(QgsMapCanvas *canvas, QObject *parent)
    : QgsMapToolEmitPoint(canvas)
  {
    setParent(parent);
    setCursor(Qt::CrossCursor);
  }
  ~PaleoNativeCoordinateMapTool() override
  {
    if (canvas() && canvas()->mapTool() == this) canvas()->unsetMapTool(this);
  }
  void activate() override
  {
    QgsMapToolEmitPoint::activate();
    if (canvas() && !m_snapIndicator) m_snapIndicator = std::make_unique<QgsSnapIndicator>(canvas());
  }
  void deactivate() override
  {
    if (m_snapIndicator) m_snapIndicator->setMatch(QgsPointLocator::Match());
    QgsMapToolEmitPoint::deactivate();
  }

signals:
  void pointPicked(const QgsPointXY &point);
  void cancelled();

protected:
  void canvasMoveEvent(QgsMapMouseEvent *event) override
  {
    if (m_snapIndicator && event)
    {
      event->snapPoint();
      m_snapIndicator->setMatch(event->mapPointMatch());
    }
    QgsMapToolEmitPoint::canvasMoveEvent(event);
  }
  void canvasReleaseEvent(QgsMapMouseEvent *event) override
  {
    if (!event || m_finished) return;
    if (event->button() == Qt::RightButton)
    {
      finish(true);
      return;
    }
    if (event->button() != Qt::LeftButton) return;
    const QgsPointXY point = event->snapPoint();
    m_finished = true;
    emit pointPicked(point);
    if (canvas()) canvas()->unsetMapTool(this);
    deleteLater();
  }
  void keyPressEvent(QKeyEvent *event) override
  {
    if (event && event->key() == Qt::Key_Escape)
    {
      finish(true);
      return;
    }
    QgsMapToolEmitPoint::keyPressEvent(event);
  }

private:
  void finish(bool wasCancelled)
  {
    if (m_finished) return;
    m_finished = true;
    if (wasCancelled) emit cancelled();
    if (canvas()) canvas()->unsetMapTool(this);
    deleteLater();
  }
  std::unique_ptr<QgsSnapIndicator> m_snapIndicator;
  bool m_finished = false;
};

PaleoCoordinateMapTool::PaleoCoordinateMapTool(QgsMapCanvas *canvas)
  : QObject(canvas), m_native(new PaleoNativeCoordinateMapTool(canvas, this))
{
  m_native->setProperty("paleoOwner", QVariant::fromValue<QObject *>(this));
  connect(m_native.data(), &PaleoNativeCoordinateMapTool::pointPicked,
          this, &PaleoCoordinateMapTool::pointPicked);
  connect(m_native.data(), &PaleoNativeCoordinateMapTool::cancelled,
          this, &PaleoCoordinateMapTool::cancelled);
}

QgsMapTool *PaleoCoordinateMapTool::nativeTool() const
{
  return m_native.data();
}

bool PaleoCoordinateMapTool::isActiveOnCanvas() const
{
  return m_native && m_native->canvas() &&
         m_native->canvas()->mapTool() == m_native.data();
}

PaleoCoordinateMapTool *PaleoCoordinateMapTool::fromMapTool(QgsMapTool *tool)
{
  return tool ? qobject_cast<PaleoCoordinateMapTool *>(
                    tool->property("paleoOwner").value<QObject *>())
              : nullptr;
}

#include "coordinatemaptool.moc"
