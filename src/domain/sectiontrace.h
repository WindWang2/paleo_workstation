// 层：数据
#pragma once
#include <QString>
#include <QVector>
#include <QtGlobal>
#include <cmath>
#include <functional>
#include <limits>

// domain/ — SEG-Y 测线级纯数据类型（自 io/segyreader.h + io/segysectiongrid.h
// 下沉）：道头几何/样本缓冲/共享时间轴网格。读取器实现（SegyReader）留在
// io/；本头只承载数据，视图/工作流/解析器三方共用。
//
// 长 IO 钩子（wave2 D1：索引/解码跑在任务池上）：progress(done,total) 报
// 字节/条目进度（worker 侧节流，~每 8MB 或每 64 道一次）；cancel() 返回真
// 即中止，函数照常返回 false、error 记 "cancelled"——任务层据此判 Cancelled。
struct SegyOptions
{
  std::function<void(qint64 done, qint64 total)> progress;
  std::function<bool()> cancel;
};

// SEG-Y rev0/1 单道样本（plan §2/§7）：索引式读取器按 inline/crossline 解码
// 填充。IBM 370 fp32（format 1）与 IEEE 754 fp32（format 5）解码保留在 io。
struct SegyTrace
{
  qint32 cdp = 0;
  qint32 lineNo = 0;   // inline（道头字节 189；0 时回退二进制头 line number）
  qint32 xlineNo = 0;  // crossline（道头字节 193，默认）
  QVector<float> samples;
  qint64 tracl = 0;
  float sampleIntervalUs = 0.0f;
  double startTimeMs = std::numeric_limits<double>::quiet_NaN();
};

struct SegyGeometry
{
  qint32 inlineMin = 0, inlineMax = 0;
  qint32 xlineMin = 0, xlineMax = 0;
  // survey 角点 (x,y)：取自 CDP 道头坐标（字节 181-188），
  // 顺序 (inlMin,xlMin) (inlMin,xlMax) (inlMax,xlMax) (inlMax,xlMin)。
  double cornerX[4] = {0, 0, 0, 0};
  double cornerY[4] = {0, 0, 0, 0};
  double startTimeMs = 0; // 道头延迟记录时间（字节 109-110，ms）
};

// Shared time axis for section previews. Each trace keeps its own sample
// interval and delay; missing portions render as neutral background.
struct SegySectionGrid
{
  double startMs = 0.0;
  double stepMs = 1.0;
  int rows = 1;

  double endMs() const { return startMs + (rows - 1) * stepMs; }

  static SegySectionGrid forTraces(const QVector<SegyTrace> &traces, float fallbackDtUs,
                                   double fallbackStartMs)
  {
    SegySectionGrid grid;
    double first = std::numeric_limits<double>::infinity();
    double last = -std::numeric_limits<double>::infinity();
    double smallestStep = std::numeric_limits<double>::infinity();
    for (const SegyTrace &trace : traces)
    {
      if (trace.samples.isEmpty()) continue;
      const double dt = (trace.sampleIntervalUs > 0 ? trace.sampleIntervalUs : fallbackDtUs) / 1000.0;
      if (dt <= 0 || !std::isfinite(dt)) continue;
      const double start = std::isfinite(trace.startTimeMs) ? trace.startTimeMs : fallbackStartMs;
      first = qMin(first, start);
      last = qMax(last, start + (trace.samples.size() - 1) * dt);
      smallestStep = qMin(smallestStep, dt);
    }
    if (!std::isfinite(first)) return grid;
    grid.startMs = first;
    const double span = last - first;
    const double requiredRows = std::ceil(span / smallestStep) + 1.0;
    grid.rows = qBound(1, static_cast<int>(qMin(8192.0, requiredRows)), 8192);
    grid.stepMs = grid.rows > 1 ? span / (grid.rows - 1) : smallestStep;
    return grid;
  }

  bool sampleAt(const SegyTrace &trace, int row, float fallbackDtUs,
                double fallbackStartMs, float *sample) const
  {
    if (!sample || trace.samples.isEmpty()) return false;
    const double dt = (trace.sampleIntervalUs > 0 ? trace.sampleIntervalUs : fallbackDtUs) / 1000.0;
    if (dt <= 0 || !std::isfinite(dt)) return false;
    const double traceStart = std::isfinite(trace.startTimeMs) ? trace.startTimeMs : fallbackStartMs;
    const double position = (startMs + row * stepMs - traceStart) / dt;
    if (position < 0 || position > trace.samples.size() - 1) return false;
    const int lo = static_cast<int>(position);
    const int hi = qMin(lo + 1, trace.samples.size() - 1);
    *sample = trace.samples.at(lo) * static_cast<float>(1.0 - (position - lo)) +
              trace.samples.at(hi) * static_cast<float>(position - lo);
    return true;
  }
};
