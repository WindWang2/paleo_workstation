// 层：数据
#pragma once
#include <QString>

// services/crashreport — 崩溃报告（PALEO_QGIS_PLAN §38 留白的落地：本地优先）。
// 决策：不联网、不回传——崩溃时异步信号安全地把报告写到本地
// <baseDir>/crash/YYYYmmdd-HHMMSS.txt；下次启动由 .running 会话旗标残留检测
// 「上次未干净退出」，主流程给诚实提示（含最近报告路径）。详见
// docs/CRASH_REPORTING.md。
//
// 落点决策：baseDir 恒为 QStandardPaths::AppDataLocation（main 启动早期解析，
// 早于任何工程打开——脏退出检测只可能在「无工程上下文」时刻发生，落点必须
// 先于工程存在）。当前工程路径写进报告头（setProjectContext），报告本身不随
// 工程搬家。本头文件只含 QString——POSIX 专用头不进公共路径（Windows 安全）。
namespace CrashReport
{

struct SessionStart
{
  bool previousDirtyExit = false; // .running 旗标残留 → 上次未干净退出
  QString lastReportPath;         // 最近一份 crash/*.txt（无则空——被 kill 也算脏退出）
};

// <baseDir>/crash（报告与会话旗标 .running 都住这里）。
QString crashDirFor(const QString &baseDir);

// 安装致命信号处理器（SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS；Windows 走 CRT
// signal() 最佳努力）+ 写 .running 旗标 + 检测上次脏退出。main() 启动早期调
// 用一次；重复调用安全（处理器幂等重装，旗标重写前先探测残留）。
// baseDir 为空/不可写 → 不装处理器、如实返回干净态（不因诊断件阻塞启动）。
SessionStart installCrashHandler(const QString &baseDir);

// 工程打开/切换时更新报告头的「当前工程路径」行（只改预渲染头，不改落点）。
void setProjectContext(const QString &projectDir);

// 正常退出清除 .running 旗标（main() 在 exec() 返回后调用；崩溃路径不清——
// 残留正是下次脏退出检测的依据）。
void clearRunningFlag();

// 最近一份报告的绝对路径（按文件名时间戳序取最新；无报告回空串）。
QString latestReportPath(const QString &baseDir);

// 同步写一份崩溃报告（信号处理器共用的同一 fd 落盘路径；测试/诊断入口，
// 不 raise）。未 install 或落盘失败 → false。
bool writeReportForSignal(int signalNumber);

// 重启提示文案（DESIGN.md 语调：平实、诚实、无装饰）。干净退出回空串；
// 脏退出必给一句话，有报告带路径、无报告如实说没有。
QString recoveryNoticeText(const SessionStart &start);

} // namespace CrashReport
