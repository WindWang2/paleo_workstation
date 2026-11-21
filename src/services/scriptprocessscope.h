// 层：数据
#pragma once
#include <QtGlobal>
class QProcess;

// services/scriptprocessscope.h — 用户脚本/REPL 的运行级进程树所有权（#960）。
// Windows：一次运行一棵树。构造时创建 KILL_ON_JOB_CLOSE 的 Job Object；
// configure() 经 QProcess 创建参数修饰器挂 PROC_THREAD_ATTRIBUTE_JOB_LIST，
// 让归属在 CreateProcess 时刻（首条脚本指令可能执行之前）生效，不在
// started 回调后补 AssignProcessToJobObject；同时用 HANDLE_LIST 把可继承
// 句柄收敛到三条标准管道。创建/属性/归属任一失败都会使 CreateProcess 失败
// （QProcess 报 FailedToStart）——绝不回退到无 Job 归属的进程。
// 不设进程/内存/CPU 上限、不启用 breakaway：显式用户脚本允许正常
// subprocess，不套用受控生成代码的单进程与资源限制口径。
// Unix：全部 no-op——调用方沿用 setsid 进程组连坐语义，不借本类重写。
// Windows 专属 API 的平台守卫 helper（cpp 内 #ifdef），其余平台无操作。
void clearProcessLaunchModifier(QProcess *process);

namespace scriptprocessscope_test
{
// 仅测试二进制调用的注入接缝（模拟 Job 创建失败）；生产代码零引用，
// 不是环境变量/配置开关，无法在产品运行时关闭隔离。
void forceCreateFailure();
void clearCreateFailure();
void forceQueryFailure();
void clearQueryFailure();
}

class ScriptProcessScope
{
public:
  ScriptProcessScope();
  ~ScriptProcessScope();
  ScriptProcessScope(const ScriptProcessScope &) = delete;
  ScriptProcessScope &operator=(const ScriptProcessScope &) = delete;

  // Job/属性内存是否就绪。false 时调用方仍须 configure()——修饰器会以
  // FailedToStart 拒绝启动，而不是悄悄退回无归属进程。
  bool isValid() const;
  // 在 proc->start() 之前挂创建期归属属性（Unix no-op）。
  void configure(QProcess &process);
  // start() 返回后查询：最近一次创建属性是否备齐（false ⇒ 启动必然失败）。
  bool launchPrepared() const;
  // 树级强停：Windows = TerminateJobObject（整树，含脚本拉起的后代）；
  // Unix 返回 false（调用方走既有进程组信号）。
  bool terminateTree();
  // Job 内是否已无存活进程（Unix 恒 true）。
  bool treeIdle() const;
  // 有界等待整树退出；超时返回 false（不无限等）。
  bool waitForTreeIdle(int timeoutMs);
  // 主进程先退（QProcess finished 先到）后的收敛：后代仍存活/持有管道时
  // 强停 + 有界等待。false = 超时仍有存活（句柄关闭仍有 KILL_ON_JOB_CLOSE
  // 内核兜底，调用方不得据此宣称排空成功）。
  bool reclaim(int timeoutMs);

private:
  struct State;
  State *m_state = nullptr; // Windows 原生状态；Unix 恒空
};
