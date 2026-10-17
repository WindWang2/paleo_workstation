// 层：QGIS 封装
#include "wellpointnavigation.h"
#include <QApplication>
#include <QMouseEvent>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <qgsmapcanvas.h>
#include <qgsmaptopixel.h>
#include <qgsgeometry.h>
#include <qgsmaptoolidentify.h>
#include <qgsvectorlayer.h>
namespace {
class Navigation final : public QObject {
public:
  Navigation(QgsMapCanvas *canvas, QgsVectorLayer *wells, QList<QgsMapTool *> tools,
             std::function<void(const QString &)> openWell)
      : QObject(canvas), m_canvas(canvas), m_wells(wells), m_tools(std::move(tools)), m_open(std::move(openWell)) {
    canvas->viewport()->installEventFilter(this);
  }
  bool eventFilter(QObject *object, QEvent *event) override {
    if (!m_wells || !m_tools.contains(m_canvas->mapTool())) { m_pressed = false; return false; }
    if (event->type() == QEvent::MouseButtonPress) {
      auto *mouse = static_cast<QMouseEvent *>(event);
      m_pressed = mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier;
      m_moved = false;
      m_press = mouse->position().toPoint();
    } else if (event->type() == QEvent::MouseMove && m_pressed) {
      m_moved |= (static_cast<QMouseEvent *>(event)->position().toPoint() - m_press).manhattanLength() >= QApplication::startDragDistance();
    } else if (event->type() == QEvent::MouseButtonRelease) {
      auto *mouse = static_cast<QMouseEvent *>(event);
      const bool click = m_pressed && !m_moved && mouse->button() == Qt::LeftButton &&
        (mouse->position().toPoint() - m_press).manhattanLength() < QApplication::startDragDistance();
      m_pressed = false;
      if (click) {
        const auto position = mouse->position().toPoint();
        // 使用点击当时的地图坐标，漫游结束或下一帧刷新不能改变这次命中的对象。
        const auto point = m_canvas->getCoordinateTransform()->toMapCoordinates(position);
        QTimer::singleShot(0, this, [this, position, point] { pick(position, point); });
      }
    }
    return QObject::eventFilter(object, event);
  }
private:
  void pick(const QPoint &position, const QgsPointXY &point) {
    if (!m_wells) return;
    QMap<QString, QString> ids;
    { // 导航可能重建正文并销毁画布，先结束原生拾取工具的生命周期。
      QgsMapToolIdentify identify(m_canvas);
      const auto hits = identify.identify(QgsGeometry::fromPointXY(point), QgsMapToolIdentify::TopDownAll, {m_wells}, QgsMapToolIdentify::VectorLayer);
      for (const auto &hit : hits) {
        const auto id = hit.mFeature.attribute("entity_id").toString();
        if (!id.isEmpty()) ids.insert(id, hit.mFeature.attribute("name").toString());
      }
    }
    const auto open = m_open;
    if (ids.size() == 1) { open(ids.firstKey()); return; }
    if (ids.isEmpty()) return;
    auto *menu = new QMenu(m_canvas);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setTitle(tr("选择此位置的井"));
    for (auto it = ids.cbegin(); it != ids.cend(); ++it) {
      auto *action = menu->addAction(tr("%1 · %2").arg(it.value(), it.key()));
      connect(action, &QAction::triggered, this, [this, open, id = it.key()] {
        QTimer::singleShot(0, this, [open, id] { open(id); });
      });
    }
    menu->popup(m_canvas->viewport()->mapToGlobal(position));
  }
  QgsMapCanvas *m_canvas;
  QPointer<QgsVectorLayer> m_wells;
  QList<QgsMapTool *> m_tools;
  std::function<void(const QString &)> m_open;
  QPoint m_press;
  bool m_pressed = false, m_moved = false;
};
}
void WellPointNavigation::install(QgsMapCanvas *canvas, QgsVectorLayer *wells,
    const QList<QgsMapTool *> &tools, const std::function<void(const QString &)> &openWell) {
  if (canvas && wells) new Navigation(canvas, wells, tools, openWell);
}
