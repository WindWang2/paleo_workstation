// 层：QGIS 封装
#pragma once

#include <memory>

// #872 诊断：QgisRuntime::initialize 的分步看门狗。
//
// 仅在 Windows 且设置了 PALEO_QGIS_INIT_TRACE 时启用（CI 的「Probe first C++
// test」步）；其余平台/未设置时为空操作，不起线程、不改任何初始化行为。
// 某一步停留超过阈值（首次 30 s，之后每 60 s）时，向 stderr 输出：
//   · 该步开始后新载入的模块（DLL 加载器/DllMain 挂死时可直接点名）；
//   · 本进程每个线程的调用栈（模块!最近导出符号+偏移），主线程在前。
// 探针挂死时 5 分钟步超时杀进程前即可拿到挂点，而不是只有「initQgis begin」。
class QgisInitWatchdog
{
  public:
    QgisInitWatchdog();
    ~QgisInitWatchdog();
    QgisInitWatchdog( const QgisInitWatchdog & ) = delete;
    QgisInitWatchdog &operator=( const QgisInitWatchdog & ) = delete;

    void enter( const char *step );
    void leave();

    struct Impl;

  private:
    std::unique_ptr<Impl> d;
};
