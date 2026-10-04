// 层：视图
#pragma once

#include "../../services/derivationgraph.h"
#include <QGraphicsView>
#include <QHash>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QGraphicsPathItem;
class QLabel;
class QLineEdit;
class QSlider;

class DerivationGraph : public QGraphicsView
{
  Q_OBJECT
public:
  explicit DerivationGraph(QWidget *parent = nullptr);
  void loadGraph(const paleo::derivation::Graph &graph);
  void highlight(const QString &selected, const QSet<QString> &closure);
  int nodeCount() const { return m_nodes.size(); }
  void fitGraph();

signals:
  void nodeClicked(const QString &versionId);

protected:
  void changeEvent(QEvent *event) override;

private:
  struct PaintedEdge { QString parent, child; QGraphicsPathItem *item = nullptr; };
  quint64 m_generation = 0;
  QHash<QString, QGraphicsItem *> m_nodes;
  QVector<PaintedEdge> m_edges;
};

// 原生过滤/深度控件只产 Query 意图；服务结果含明确的折叠/空态说明。
class DerivationPanel : public QWidget
{
  Q_OBJECT
public:
  explicit DerivationPanel(QWidget *parent = nullptr);
  paleo::derivation::Query query() const;
  void setGraph(const paleo::derivation::Graph &graph);
  DerivationGraph *graphView() const { return m_graph; }

signals:
  void queryChanged();
  void nodeClicked(const QString &versionId);

private:
  QWidget *m_controls = nullptr;
  QSlider *m_up = nullptr, *m_down = nullptr;
  QComboBox *m_entity = nullptr, *m_kind = nullptr;
  QLineEdit *m_asset = nullptr;
  QCheckBox *m_stale = nullptr;
  QLabel *m_summary = nullptr;
  DerivationGraph *m_graph = nullptr;
};
