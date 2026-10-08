// 层：数据
#pragma once
#include <QByteArray>
#include <QString>
#include <QtGlobal>

// metadata/processprobe — 锁持有者存活探测（方向 81 pid 真修）。
//
// 背景：ProjectDirLock 依赖 QLockFile 的陈旧锁回收，而 Qt 在 Windows 上判
// 「同一台机器」只拿锁文件主机行与环境变量 COMPUTERNAME 做大小写敏感全等
// （qlockfile.cpp machineName()；POSIX 侧是 QSysInfo::machineHostName()）。
// 锁文件没有 hostid 行（Qt<5.10 旧格式/手写/外部工具写的锁）且主机行是 DNS
// 口径、大小写不同或写入进程的 COMPUTERNAME 被裁掉时，Qt 永远判「别的机器」
// ——pid 早已不存在也不回收（方向 72 R9 实测：pid 128364 不存在仍被判存活）。
// 本模块提供与 Qt 解耦的两个判据，供 ProjectDirLock 在 Qt 拒绝后兜底复核。
namespace paleo::proc
{

enum class ProcessState
{
  Alive,   // 进程存在（含：存在但无权查询/发信号——保守按存活）
  Dead,    // 确认不存在（或已退出、仅剩被他人句柄引用的进程对象）
  Unknown, // pid 非法或探测失败——调用方不得据此回收任何东西
};

// 跨平台 pid 存活探测：
//   POSIX  ：kill(pid, 0)；0 → Alive，ESRCH → Dead，EPERM → Alive。
//   Windows：OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE)；
//            ERROR_INVALID_PARAMETER → Dead，ERROR_ACCESS_DENIED → Alive；
//            拿到句柄后 WaitForSingleObject(h, 0)：WAIT_TIMEOUT → Alive，
//            WAIT_OBJECT_0 → Dead（不用 GetExitCodeProcess==STILL_ACTIVE：
//            退出码恰为 259 的已死进程会被误判存活）。
ProcessState probeProcess( qint64 pid );

// 锁文件里的主机标识是否指本机。
//   · lockHostId（QLockFile 第 4 行 machineUniqueId）非空且本机 id 可得 →
//     只认 id 全等（跨机共享盘上绝不按名字放宽）；
//   · 否则按主机名：空 → 本机（与 Qt 同义）；非空则与本机候选名（POSIX:
//     machineHostName；Windows: COMPUTERNAME 环境变量 + GetComputerNameEx
//     的 NetBIOS/DNS 名，不依赖环境变量）做大小写不敏感比较，全名或首段
//     （短名，去域后缀）相等即本机。
bool lockHostIsThisMachine( const QString &lockHostName, const QByteArray &lockHostId );

} // namespace paleo::proc
