// 层：数据
#pragma once
#include "../domain/sectiontrace.h" // SegyReadReport/SegyTrace（SectionDoc 值成员）

#include "previewdoc.h"

// services/ — PreviewDocService::SectionDoc 的完整定义（方向 59 从
// previewdoc.h 拆出的重出参类型）。previewdoc.h 只留嵌套前向声明
// （信号/引用形参够用）；要触碰成员（doc.traces 等）或注册 metatype
// 的消费 TU 自行 include 本头。拆出动机：SectionDoc 的值成员需要
// domain/sectiontrace.h 完整类型，留在门面头会把该重头灌进全部消费
// TU（方向 59 前实测 27 个 ui TU 因 previewdoc.h 被动携带重编面）。

// 剖面解码结果（seismicSectionReady 的载荷）。结果经信号跨线程交接
// （任务池路径）——Q_DECLARE_METATYPE 供排队连接/QSignalSpy（运行期
// 注册在 PreviewDocService 构造函数里）。
struct PreviewDocService::SectionDoc
{
  SegyReadReport readReport;
  QVector<SegyTrace> traces;
  float sampleIntervalUs = 0.0f;
  double startTimeMs = 0.0;
  bool isInline = true;
  int lineNo = -1;
};

Q_DECLARE_METATYPE(PreviewDocService::SectionDoc)
