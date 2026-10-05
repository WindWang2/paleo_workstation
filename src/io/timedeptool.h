// 层：数据
#pragma once
#include "wellfileparsers.h"

// io/ — 时深工具（PROJECT_AREA_PLAN §3 autoplan）：TVD 对 TD 表 TVD 列线性插值
// 得 TIME(ms)；井分层 TVD 空时用 MD 对 MD 列。契约：
//   · 行保持文件顺序——不排序、不夹取、不外推；
//   · -99999/缺列的行不进插值（在单调性与样点数检查之前剔除）；
//   · 查找列（TVD 或 MD）必须按文件顺序严格递增 → 否则 NonMonotonic「时深表无序」；
//   · 可用样点不足两个 → NoTable「无时深表」；
//   · 深度落在 [首可用样点, 末可用样点] 之外 → OutOfRange「超出时深表」，
//     绝不返回端点值；
//   · 范围内 → 文件顺序相邻两样点间线性插值 → Ok + ms。
// 剖面标层与时间残差共用这一个结果：要么值，要么原因（reasonText 取中文文案）。
namespace TimeDepthTool
{
  enum class TdStatus
  {
    Ok,           // 插值成功，timeMs 有效
    NoTable,      // 无时深表（无 TD 行，或剔除 -99999 后不足两个可用样点）
    OutOfRange,   // 超出时深表（深度在可用样点范围之外，不外推）
    NonMonotonic, // 时深表无序（查找列未按文件顺序严格递增）
  };

  struct TdResult
  {
    double timeMs = qQNaN();              // 仅 status==Ok 时有效
    TdStatus status = TdStatus::NoTable;
    bool ok() const { return status == TdStatus::Ok; }
  };

  // depth→time(ms)。useMd=false 查 TVD 列（井分层 TVD 有效时的首选）；
  // useMd=true 查 MD 列（分层 TVD 空的兜底）。失败原因见 status/reasonText。
  TdResult interpolateTimeMs(const TimeDepthTable &td, double depth, bool useMd);

  // 方向 44 逆插值：time(ms)→depth（米）。preferMd=true 取 MD 列
  // （对齐到 MD 基准），false 取 TVD 列。契约与 interpolateTimeMs 同源：
  // 文件顺序、timeMs 查找列严格递增、样点<2 → NoTable、范围外 → OutOfRange
  // 绝不外推。井曲线 TIME 基准对齐（petrophys 并集）走这里。
  struct TdDepthResult
  {
    double depth = qQNaN();               // 仅 status==Ok 时有效（米）
    TdStatus status = TdStatus::NoTable;
    bool ok() const { return status == TdStatus::Ok; }
  };
  TdDepthResult interpolateDepthAtTimeMs(const TimeDepthTable &td,
                                         double timeMs, bool preferMd);

  // 原因文案：Ok → 空串；否则「无时深表」「超出时深表」「时深表无序」之一。
  QString reasonText(TdStatus status);
} // namespace TimeDepthTool
