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
  // 训练可用态重算：按当前方法组装 options → validateTrainingSet → 面板三态；
  // 同步刷新标注摘要。标注变更/方法切换/换样本后都走这里。
  void refreshTrainingState();
  AppContext *m_context;
  CrossplotPanel *m_panel;
  FaciesClassifyWorkflow *m_workflow;
  CrossplotMapLink *m_map;
  QVector<SourceSpec> m_sources;
  PlotFrame m_frame;
  QPointer<PaleoTask> m_loadTask;
  quint64 m_generation = 0;
  bool m_refreshPending = false;
  // 当前套索选区（assignLabelRequested 取 indices；project/load 时清空）。
  Selection m_selection;
  // 最近一次成功训练的 report 与其 supervisedMethod（methodChanged 切回已
  // 训练方法时回填质量区；模型陈旧后回填即失配——由 m_trainedMethod 守卫）。
  QVariantMap m_lastReport;
  // 已训练模型的 Classifier 枚举值；-1 = 无模型或已陈旧/已清（classify 门禁
  // 同语义：训练集变更即要求重训）。方法切换时按与此值是否相等刷新已训练态。
  int m_trainedMethod = -1;
};
} // namespace paleo::crossplot
