// 层：数据
#include "startuptrace.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>

#include <QGlobalStatic>

#include <cstdio>
#include <cstdlib>

#if defined(Q_OS_LINUX)
#include <QFile>
#include <unistd.h> // sysconf/_SC_CLK_TCK
#endif

#include <utility> // std::as_const

namespace
{
struct TraceState
{
  QMutex mutex;
  QElapsedTimer clock;
  bool started = false;
  bool finished = false;
  double processToMainMs = -1.0;
  QVector<StartupTrace::Segment> segments;

  bool ensureStarted()
  {
    if (!started)
    {
      clock.start();
      started = true;
    }
    return started;
  }
};
} // namespace

Q_GLOBAL_STATIC(TraceState, s_trace)

namespace StartupTrace
{
namespace
{
// /proc/self/stat 字段 22 = starttime（clock ticks since boot）；
// /proc/uptime 首值 = 秒 since boot。差值 ×（1000 / CLK_TCK）即
// exec→main 的墙钟（动态链接 + 重定位 + 静态初始化）。读不到返回 -1，
// 不臆造。
double readProcessToMainMs()
{
#if defined(Q_OS_LINUX)
  QFile stat(QStringLiteral("/proc/self/stat"));
  if (!stat.open(QIODevice::ReadOnly))
    return -1.0;
  const QByteArray raw = stat.readAll();
  // 字段 2 comm 可能含空格/括号——从最后的 ')' 之后起算字段 3。
  const int closeParen = raw.lastIndexOf(')');
  if (closeParen < 0 || closeParen + 1 >= raw.size())
    return -1.0;
  const QList<QByteArray> fields =
      raw.mid(closeParen + 2).split(' '); // fields[0] = 字段 3 (state)
  // 字段 22 = index 19（state 起 20 个字段偏移：22-3）。
  if (fields.size() < 20)
    return -1.0;
  bool ok = false;
  const long long startTicks = fields.at(19).toLongLong(&ok);
  if (!ok || startTicks <= 0)
    return -1.0;
  QFile uptime(QStringLiteral("/proc/uptime"));
  if (!uptime.open(QIODevice::ReadOnly))
    return -1.0;
  const double upSecs = uptime.readAll().split(' ').value(0).toDouble(&ok);
  if (!ok || upSecs <= 0)
    return -1.0;
  const double ticksPerSec = sysconf(_SC_CLK_TCK);
  if (ticksPerSec <= 0)
    return -1.0;
  const double elapsedMs = (upSecs - startTicks / ticksPerSec) * 1000.0;
  return elapsedMs >= 0.0 && elapsedMs < 3600.0 * 1000.0 ? elapsedMs : -1.0;
#else
  return -1.0;
#endif
}
} // namespace

void mark(const QString &name)
{
  TraceState *t = s_trace;
  if (!t)
    return;
  QMutexLocker lock(&t->mutex);
  t->ensureStarted();
  if (!t->segments.isEmpty() && t->segments.last().name == name)
    return; // 相邻同名重复打点：一次为止（双记会把段间时长算没）
  Segment seg;
  seg.name = name;
  seg.atMs = double(t->clock.nsecsElapsed()) / 1.0e6;
  t->segments.append(seg);
}

bool finish()
{
  TraceState *t = s_trace;
  if (!t)
    return false;
  const QString path =
      QString::fromLocal8Bit(qgetenv("PALEO_STARTUP_TRACE"));
  {
    QMutexLocker lock(&t->mutex);
    if (t->finished || path.isEmpty())
      return false;
    if (t->processToMainMs < 0.0)
      t->processToMainMs = readProcessToMainMs();
    t->finished = true;
  }
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    std::fprintf(stderr, "paleo: PALEO_STARTUP_TRACE 无法写入 %s\n",
                 qPrintable(path));
    return false;
  }
  f.write(toJson());
  return true;
}

QVector<Segment> segments()
{
  TraceState *t = s_trace;
  if (!t)
    return {};
  QMutexLocker lock(&t->mutex);
  return t->segments;
}

double processStartToMainMs()
{
  TraceState *t = s_trace;
  if (!t)
    return -1.0;
  QMutexLocker lock(&t->mutex);
  if (!t->ensureStarted())
    return -1.0;
  if (t->processToMainMs < 0.0)
    t->processToMainMs = readProcessToMainMs();
  return t->processToMainMs;
}

void resetForTest()
{
  TraceState *t = s_trace;
  if (!t)
    return;
  QMutexLocker lock(&t->mutex);
  t->started = false;
  t->finished = false;
  t->processToMainMs = -1.0;
  t->segments.clear();
}

QByteArray toJson()
{
  TraceState *t = s_trace;
  if (!t)
    return {};
  QMutexLocker lock(&t->mutex);
  t->ensureStarted();
  QJsonObject root;
  root.insert(QStringLiteral("process_to_main_ms"),
              t->processToMainMs >= 0.0 ? QJsonValue(t->processToMainMs)
                                        : QJsonValue(-1.0));
  QJsonArray segs;
  for (const Segment &s : std::as_const(t->segments))
  {
    QJsonObject o;
    o.insert(QStringLiteral("name"), s.name);
    o.insert(QStringLiteral("at_ms"), s.atMs);
    segs.append(o);
  }
  root.insert(QStringLiteral("segments"), segs);
  root.insert(QStringLiteral("total_ms"),
              double(t->clock.nsecsElapsed()) / 1.0e6);
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}
} // namespace StartupTrace
