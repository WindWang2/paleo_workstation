// 层：视图
#pragma once
// paleomainwindow_runners — #558 扇出根一拆解的私有伴生头：主窗口侧
// 「在途作业状态」（JobRunner + 交接体）的完整类型只在这里展开，
// 不再经 paleomainwindow.h 把 workflow 头 → algorithms/io 重头链扇出给
// 全体 ui TU（旧形态：值成员需完整类型，单头受染 29-36 TU）。
// 仅 PaleoMainWindow 实现侧（paleomainwindow_attach_models.cpp）
// 允许 include——这是实现细节，不是公共面；新 ui 代码勿引本头。

#include <QObject>

#include "../services/jobrunner.h"
#include "../workflow/propertymodelworkflow.h"
#include "../workflow/faciesmappingworkflow.h"

#include <memory>

// 属性建模在途作业的窗口侧状态（#85/方向20/V2）：runner 承担忙则拒绝/
// 取消传播/commit 线程亲和；job 是与 commit 段共享的交接体（shared_ptr
// 共享所有权）；computed 是无任务池兜底路径（旧测试壳）的同步结果表。
// 生命周期：本对象是主窗口的 QObject 子对象，runner 以主窗口为
// dispatcher/Qt 父——全部由 Qt 父子链回收（懒建点见 propModelRun()），
// 主窗口析构（无事件循环）期间连接自动断开，无悬垂。
struct PropModelRunState : QObject
{
  explicit PropModelRunState(QObject *window)
      : QObject(window), runner(window)
  {
  }

  paleo::jobs::JobRunner<PropertyModelWorkflow::PropertyModelJob> runner;
  std::shared_ptr<PropertyModelWorkflow::PropertyModelJob> job;
  PropertyModelWorkflow::PropertyModelComputedList computed; // 无池兜底路径
};

// 相图合成在途作业的窗口侧状态（goal/facies-automapping，同上形态）。
struct FaciesMappingRunState : QObject
{
  explicit FaciesMappingRunState(QObject *window)
      : QObject(window), runner(window)
  {
  }

  paleo::jobs::JobRunner<FaciesMappingWorkflow::DraftFaciesJob> runner;
  std::shared_ptr<FaciesMappingWorkflow::DraftFaciesJob> job;
};
