// 层：数据
#pragma once

#include <QString>
#include <QVector>

// goal/perf-systematize 簇1：启动分段仪表（main 起点计时，各阶段 mark 打点）。
//
// 语义：
//   · 首次 mark 前自动起步（无需显式 begin；main 第一行 mark("main_entry")）。
//   · mark 恒记录（一次 QElapsedTimer::elapsed + vector append，纳秒级）；
//     落盘才受 env 门控——产品路径（PALEO_STARTUP_TRACE 未设）finish()
//     直接 no-op，零可观测开销。
//   · process→main 段读 /proc/self/stat starttime 与 /proc/uptime 之差
//     （动态链接器 + 重定位 + 静态初始化的真实墙钟——LTO/PGO 启动实验的
//     观测面）；非 Linux 或读不到 → -1，不补假值。
//   · finish() 把 JSON 写到 PALEO_STARTUP_TRACE 路径并复位仪表（一个进程
//     只落一次盘；重复调用返回 false）。
//
// 分段词表（tst_startup_trace 断言次序；新增段先扩测试）：
//   main_entry → pre_qt_ready → qgis_app_ready → services_ready →
//   theme_ready → main_window_ready → window_shown → first_paint
namespace StartupTrace
{
struct Segment
{
  QString name;
  double atMs = 0.0; // 距 main_entry 的毫秒数
};

// 打点（线程安全；全量调用在 main 线程）。重复名追加不覆盖——消费侧按
// 首次出现取段（防同段双记把时长算虚高）。
void mark(const QString &name);

// 末段落盘：写 PALEO_STARTUP_TRACE（未设 → no-op 返回 false）。幂等。
bool finish();

// 测试观察面（进程内断言用；未起步返回空）。
QVector<Segment> segments();
double processStartToMainMs(); // <0 = 未知（非 Linux / /proc 不可读）
void resetForTest();           // 仅测试进程用：清全部状态

// JSON 序列化（finish 与测试共用同一形状）。
QByteArray toJson();
} // namespace StartupTrace
