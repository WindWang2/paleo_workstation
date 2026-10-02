// 层：组装根
#pragma once
#include "domain/faciesclassification.h"
#include "services/crossplotsources.h"
#include <QObject>
#include <QPointer>
class AppContext;
class PaleoMainWindow;
class PaleoTask;
namespace paleo::crossplot {
class CrossplotPanel;
class FaciesClassifyWorkflow;
class CrossplotMapLink;
class CrossplotController : public QObject {
  Q_OBJECT
public:
  CrossplotController(AppContext *, PaleoMainWindow *);

private:
  void refreshSources();
  void load(const QStringList &);
  void project(const Axes &);
  void select(const QVector<QPointF> &);
  void locate(QPointF);
  AppContext *m_context;
  CrossplotPanel *m_panel;
  FaciesClassifyWorkflow *m_workflow;
  CrossplotMapLink *m_map;
  QVector<SourceSpec> m_sources;
  PlotFrame m_frame;
  QPointer<PaleoTask> m_loadTask;
  quint64 m_generation = 0;
  bool m_refreshPending = false;
};
} // namespace paleo::crossplot
