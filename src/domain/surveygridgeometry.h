// 层：数据
#pragma once
#include <QString>

struct HorizonHeader;

// ---- 测网几何：map XY ↔ (inline, crossline) --------------------------------
// P1/P2/P3 是三个角锚 (inline, xline, x, y)：P1→P2 沿 crossline 方向、
// P2→P3 沿 inline 方向（binHorizon 已验证与 Grid_size 一致）。仿射：
//   x = p1x + a·(xl − p1Xline) + b·(inl − p1Inline)
//   y = p1y + c·(xl − p1Xline) + d·(inl − p1Inline)
// 支持斜测网（a..c 非零交叉项）；本工区为轴对齐（b=c=0）。
struct SurveyGridGeometry
{
  int inlineMin = 0, inlineMax = 0, xlineMin = 0, xlineMax = 0;
  double a = 0.0, b = 0.0, c = 0.0, d = 0.0; // 仿射系数（见上）
  double p1x = 0.0, p1y = 0.0;
  int p1Inline = 0, p1Xline = 0;
  // R3-7：线号步长（默认 1）。由层位头 Grid_size 与锚点跨度整除推得（如
  // 1000,1002,… 步长 2）；推不出时保持 1（旧口径）。整数化时按 p1 + k·step 取最近格线。
  int inlineStep = 1, xlineStep = 1;
  bool valid = false;

  static SurveyGridGeometry fromHorizonHeader( const HorizonHeader &h );

  // 连续坐标正反解（不判范围；未构造 valid 时行为未定义——先查 valid）。
  void inlineXlineToXy( double inlineNo, double xline, double *x, double *y ) const;
  void xyToInlineXlineContinuous( double x, double y, double *inlineNo, double *xline ) const;

  // 整数化 + 范围门：最近格点语义——连续解四舍五入到最近 (inline, crossline)
  // 后必须在 [min,max] 内（含边界；网缘半道内自然归边缘道，与像元采样的
  // 半开区间口径互补：定位用最近道，取值用包含像元）。
  // 网外 → false + reason（含坐标与测网范围），输出参数不动——绝不夹取。
  bool xyToInlineXline( double x, double y, int *inlineNo, int *xline,
                        QString *reason = nullptr ) const;
  bool inlineXlineInside( int inlineNo, int xline ) const;
};
