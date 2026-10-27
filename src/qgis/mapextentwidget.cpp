// 层：QGIS 封装
#include "mapextentwidget.h"
#include "crsselector.h"
#include "coordinateformat.h"

#include <QComboBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#include <qgsextentwidget.h>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>

namespace
{
QgsRectangle rectangle(const PaleoMapExtent &area)
{
  return area.valid() ? QgsRectangle(area.xMin, area.yMin, area.xMax, area.yMax) : QgsRectangle();
}
}

PaleoMapExtentWidget::PaleoMapExtentWidget(QWidget *parent)
  : QObject(parent), m_widget(new QWidget(parent))
{
  auto *layout = new QVBoxLayout(m_widget);
  layout->setContentsMargins(0, 0, 0, 0);
  m_native = new QgsExtentWidget(m_widget, QgsExtentWidget::ExpandedStyle);
  m_native->setObjectName(QStringLiteral("mapbookExtent"));
  m_native->setNullValueAllowed(true, tr("未设置范围"));
  m_native->setOutputCrs(paleo::coordinates::engineeringGrid());
  m_native->setToolTip(tr("手工输入 xmin, ymin, xmax, ymax；也可从当前视图或地图框选。坐标单位随输出坐标系。"));
  layout->addWidget(m_native);
  m_layers = new QComboBox(m_widget);
  m_layers->setObjectName(QStringLiteral("mapbookExtentLayer"));
  m_layers->setEnabled(false);
  m_layers->setToolTip(tr("地图画布就绪后，可按实际图层范围确定 AOI，并转换到输出坐标系。"));
  layout->addWidget(m_layers);
  m_status = new QLabel(m_widget);
  m_status->setObjectName(QStringLiteral("mapbookExtentStatus"));
  m_status->setWordWrap(true);
  m_status->setText(tr("范围来自当前地图坐标系；不会按经纬度数值猜测轴序。"));
  layout->addWidget(m_status);
  connect(m_native, &QgsExtentWidget::extentChanged, this, [this](const QgsRectangle &) {
    emit extentChanged();
  });
  connect(m_native, &QgsExtentWidget::validationChanged, this, [this](bool valid) {
    m_status->setText(valid ? tr("范围有效；数值按输出坐标系解释，不猜测经纬度或轴序。")
                            : tr("请输入有效的 xmin, ymin, xmax, ymax，且右上限大于左下限。"));
  });
  connect(m_native, &QgsExtentWidget::toggleDialogVisibility,
          this, &PaleoMapExtentWidget::canvasVisibilityRequested);
  connect(m_layers, &QComboBox::activated, this, [this](int index) {
    auto *canvas = qobject_cast<QgsMapCanvas *>(m_canvas);
    if (!canvas || index <= 0) return;
    const QString id = m_layers->itemData(index).toString();
    for (auto *layer : canvas->layers())
    {
      if (layer && layer->id() == id)
      {
        try
        {
          QgsCoordinateTransform transform(layer->crs(), m_native->outputCrs(),
                                           canvas->mapSettings().transformContext());
          if (!transform.isValid())
          {
            m_status->setText(tr("无法建立图层到输出坐标系的转换，未改变 AOI。"));
            return;
          }
          const auto extent = transform.transformBoundingBox(layer->extent());
          if (!PaleoMapExtent{extent.xMinimum(), extent.yMinimum(),
                              extent.xMaximum(), extent.yMaximum()}.valid())
          {
            m_status->setText(tr("图层范围转换返回无效范围，未改变 AOI。"));
            return;
          }
          m_native->setOutputExtentFromUser(extent, m_native->outputCrs());
          m_status->setText(tr("已读取“%1”的实际范围，并按当前工程上下文转换。").arg(layer->name()));
        }
        catch (const QgsCsException &exception)
        {
          m_status->setText(tr("图层范围转换失败：%1").arg(exception.what()));
        }
        return;
      }
    }
    m_status->setText(tr("图层已移除，请重新选择范围。"));
  });
}

PaleoMapExtentWidget::~PaleoMapExtentWidget()
{
  delete m_widget.data();
}

QWidget *PaleoMapExtentWidget::widget() const
{
  return m_widget.data();
}

PaleoMapExtent PaleoMapExtentWidget::area() const
{
  const auto extent = m_native->outputExtent();
  return {extent.xMinimum(), extent.yMinimum(), extent.xMaximum(), extent.yMaximum()};
}

void PaleoMapExtentWidget::setArea(const PaleoMapExtent &area)
{
  if (!area.valid())
  {
    m_native->clear();
    m_status->setText(tr("尚未设置 AOI 范围。可从当前视图、实际图层或画布框选。"));
    return;
  }
  m_native->setOutputExtentFromUser(rectangle(area), m_native->outputCrs());
  m_status->setText(tr("已设置 AOI 范围；可从当前视图、实际图层或画布框选调整。"));
}

void PaleoMapExtentWidget::setCanvas(QWidget *canvas)
{
  if (m_canvas == canvas) return;
  if (auto *previous = qobject_cast<QgsMapCanvas *>(m_canvas)) disconnect(previous, nullptr, this, nullptr);
  m_canvas = canvas;
  auto *mapCanvas = qobject_cast<QgsMapCanvas *>(canvas);
  m_layers->setEnabled(mapCanvas != nullptr);
  if (!mapCanvas)
  {
    m_layers->clear();
    return;
  }
  const auto crs = mapCanvas->mapSettings().destinationCrs();
  m_native->setMapCanvas(mapCanvas);
  m_native->setOutputCrs(crs);
  m_native->setOriginalExtent(mapCanvas->fullExtent(), crs);
  m_native->setCurrentExtent(mapCanvas->extent(), crs);
  if (!area().valid()) m_native->setOutputExtentFromCurrent();
  connect(mapCanvas, &QgsMapCanvas::extentsChanged, this, [this, mapCanvas] {
    m_native->setCurrentExtent(mapCanvas->extent(), mapCanvas->mapSettings().destinationCrs());
  });
  connect(mapCanvas, &QgsMapCanvas::destinationCrsChanged, this, [this, mapCanvas] {
    const auto &crs = mapCanvas->mapSettings().destinationCrs();
    m_native->setOutputCrs(crs);
    m_native->setOriginalExtent(mapCanvas->fullExtent(), crs);
    m_native->setCurrentExtent(mapCanvas->extent(), crs);
  });
  connect(mapCanvas, &QgsMapCanvas::layersChanged, this, &PaleoMapExtentWidget::refreshLayers);
  refreshLayers();
}

void PaleoMapExtentWidget::setTargetCrsDefinition(const QString &definition)
{
  const auto crs = paleo::coordinates::crsFromDefinition(definition);
  if (crs.isValid()) m_native->setOutputCrs(crs);
}

void PaleoMapExtentWidget::refreshLayers()
{
  m_layers->clear();
  m_layers->addItem(tr("按实际图层范围…"));
  auto *canvas = qobject_cast<QgsMapCanvas *>(m_canvas);
  if (!canvas) return;
  m_native->setOriginalExtent(canvas->fullExtent(), canvas->mapSettings().destinationCrs());
  for (auto *layer : canvas->layers())
    if (layer) m_layers->addItem(layer->name(), layer->id());
}
