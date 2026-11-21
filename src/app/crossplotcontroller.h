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
  // 通道清单刷新（A-W3/W4）：所有入口（构造、projectOpened、catalog 变化、
  // 显式打开、tab 激活）进同一异步调度——catalog 快照在 GUI 线程取，表头
  // 读取与就绪判定跑在共享任务服务的 Document scope 任务里；面板可见用
  // 交互优先级，隐藏预热用低优先级（quiet）。在途任务 + 1 个待处理封顶，
  // 不积累整批 worker。旧同步整本表头读取路径不再从 GUI 触发。
  void scheduleSourceRefresh(bool interactive);
  void startSourceRefresh(bool interactive);
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
  QPointer<PaleoTask> m_sourceTask;
  quint64 m_generation = 0;
  quint64 m_sourceEpoch = 0;      // 工程切换推进：旧清单结果不得应用
  bool m_sourceRefreshPending = false; // 在途时合并出的「最新待处理」（≤1）
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
