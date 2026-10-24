// 层：视图
#pragma once
#include "../../services/datagraph.h"
#include "../paleotheme.h"
#include <QCoreApplication>
#include <QGraphicsObject>
#include <QGraphicsSceneMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
using namespace paleo::datagraph;
class DataGraphNodeItem final : public QGraphicsObject {
public:
  Node node;
  std::function<void(const Node &, bool)> selected;
  std::function<void(const Node &)> activated;
  bool compact = false;
  explicit DataGraphNodeItem(const Node &n, bool network = false) : node(n), compact(network) {
    setFlags(ItemIsSelectable | ItemIsFocusable);
    setCursor(Qt::PointingHandCursor);
    setZValue(1);
    updateNode(n);
  }
  QRectF boundingRect() const override {
    return compact ? QRectF(0, 0, 160, node.kind == Kind::Entity ? 80 : 56)
                   : QRectF(0, 0, 260, node.kind == Kind::Entity ? 112 : 88);
  }
  void setCompact(bool value) {
    if (compact == value) return;
    prepareGeometryChange(); compact = value; update();
  }
  void updateNode(const Node &n) {
    node = n;
    setPos(n.position);
    setToolTip(
        (n.title + QLatin1Char('\n') + n.detail + QLatin1Char('\n') + n.path)
            .toHtmlEscaped());
    update();
  }
  void paint(QPainter *p, const QStyleOptionGraphicsItem *,
             QWidget *) override {
    const auto &t = PaleoTheme::tokens();
    p->setPen(QPen(isSelected() || hasFocus()                    ? t.focusRing
                   : node.stale || node.unresolved || node.cycle ? t.warning
                                                                 : t.border,
                   isSelected() || hasFocus() ? 2 : 1));
    const bool collection = node.kind == Kind::Entity;
    p->setBrush(collection || node.kind == Kind::Group ? t.surfaceAlt : t.surface);
    if (collection) p->drawEllipse(boundingRect().adjusted(1, 1, -1, -1));
    else p->drawRoundedRect(boundingRect().adjusted(1, 1, -1, -1), t.radiusSm, t.radiusSm);
    QFont font = p->font();
    font.setPointSize(t.bodyPt);
    font.setBold(collection || node.kind == Kind::Group);
    p->setFont(font);
    p->setPen(t.text);
    const qreal lod = QStyleOptionGraphicsItem::levelOfDetailFromTransform(p->worldTransform());
    if (compact) {
      if (lod < .35) return;
      const auto box = boundingRect().adjusted(t.spacingSm, t.spacingSm, -t.spacingSm, -t.spacingSm);
      p->drawText(box.adjusted(0, 0, 0, -box.height() / 2), Qt::AlignCenter,
          QFontMetrics(font).elidedText(node.title, Qt::ElideMiddle, int(box.width())));
      if (lod < .65) return;
      font.setBold(false); font.setPointSize(t.labelPt); p->setFont(font); p->setPen(t.textMuted);
      const QString detail = node.unresolved ? QCoreApplication::translate("DataGraphPage", "待关联")
          : node.stale ? QCoreApplication::translate("DataGraphPage", "已过期")
          : node.cycle ? QCoreApplication::translate("DataGraphPage", "血缘环需核查")
          : collection ? QCoreApplication::translate("DataGraphPage", "%1 份文件").arg(node.count) : stageText(node.stage);
      p->drawText(box.adjusted(0, box.height() / 2, 0, 0), Qt::AlignCenter,
          QFontMetrics(font).elidedText(detail, Qt::ElideRight, int(box.width())));
      return;
    }
    if (lod < .35)
      return;
    p->drawText(
        QRectF(collection ? 26 : t.spacingSm, collection ? 20 : t.spacingSm, collection ? 208 : 244, 26), collection ? Qt::AlignCenter : Qt::AlignVCenter,
        QFontMetrics(font).elidedText(node.title, Qt::ElideMiddle, collection ? 208 : 244));
    if (lod < .65)
      return;
    font.setBold(false);
    font.setPointSize(t.labelPt);
    p->setFont(font);
    p->setPen(t.textMuted);
    QString kind =
        node.kind == Kind::Entity
            ? QCoreApplication::translate("DataGraphPage", "资料集合")
        : node.kind == Kind::Asset
            ? QCoreApplication::translate("DataGraphPage", "文件资料")
        : node.kind == Kind::Version
            ? QCoreApplication::translate("DataGraphPage", "资料版本")
        : node.kind == Kind::Activity
            ? QCoreApplication::translate("DataGraphPage", "生成记录")
        : node.kind == Kind::Missing
            ? QCoreApplication::translate("DataGraphPage", "待核实")
        : node.kind == Kind::Evidence ? QCoreApplication::translate("DataGraphPage", "地质证据")
        : node.kind == Kind::Assertion ? QCoreApplication::translate("DataGraphPage", "地质判断")
        : node.kind == Kind::Interpretation ? QCoreApplication::translate("DataGraphPage", "地质解释")
        : node.kind == Kind::Feature ? QCoreApplication::translate("DataGraphPage", "地图图斑")
        : node.kind == Kind::ReviewEvent ? QCoreApplication::translate("DataGraphPage", "审核事件")
            : QCoreApplication::translate("DataGraphPage", "分组");
    p->drawText(QRectF(collection ? 26 : t.spacingSm, collection ? 51 : 36, collection ? 208 : 244, 19),
                QFontMetrics(font).elidedText((collection ? QString() : kind + QStringLiteral(" · ")) + node.detail,
                                              Qt::ElideRight, collection ? 208 : 244));
    QString state =
        collection ? QCoreApplication::translate("DataGraphPage", "双击查看集合内文件") : node.kind == Kind::Asset ? stageText(node.stage) : node.path;
    if (node.stale)
      state += QCoreApplication::translate("DataGraphPage", " · 已过期");
    if (node.unresolved)
      state += QCoreApplication::translate("DataGraphPage", " · 待关联");
    p->drawText(QRectF(collection ? 26 : t.spacingSm, collection ? 78 : 58, collection ? 208 : 244, 19),
                QFontMetrics(font).elidedText(state, Qt::ElideMiddle, 244));
  }
  void mousePressEvent(QGraphicsSceneMouseEvent *e) override {
    QGraphicsObject::mousePressEvent(e);
    dispatch(false);
  }
  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) override {
    e->accept();
    dispatch(true);
  }
  void keyPressEvent(QKeyEvent *e) override {
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
      dispatch(true);
      e->accept();
    } else if (e->key() == Qt::Key_Space) {
      dispatch(false);
      e->accept();
    } else
      QGraphicsObject::keyPressEvent(e);
  }

private:
  void dispatch(bool open) {
    // Selection can refresh other panels and eventually rebuild the scene;
    // leave item event stack first.
    const Node value = node;
    const auto pick = selected;
    const auto activate = activated;
    QTimer::singleShot(0, this, [value, pick, activate, open] {
      if (open) {
        if (activate)
          activate(value);
      } else if (pick)
        pick(value, true);
    });
  }
};

class QGraphicsPathItem;
class QGraphicsScene;
QGraphicsPathItem *addDataGraphEdge(QGraphicsScene *scene, QGraphicsItem *source, QGraphicsItem *target, const paleo::datagraph::Edge &edge, double bend = 0);

void updateDataGraphEdgeLabels(QGraphicsPathItem *edge);
