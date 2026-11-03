// 层：数据
#pragma once
#include <QString>
#include <QtNumeric>

namespace paleo::singlefactor {
struct GrSandOptions {
  double sandGr=qQNaN(), shaleGr=qQNaN(); // 缺省用本条全井有效 GR 的 P05/P95。
  double shaleCutoff=.5;
  double maxGapMetres=0; // 0：本条曲线正深度步长中位数的三倍。
};
struct GrSandEstimate {
  bool ok=false;
  QString reason;
  double ratio=qQNaN(), sandMetres=0, coveredMetres=0, coverage=0;
  double sandGr=qQNaN(), shaleGr=qQNaN(), thresholdGr=qQNaN(), maxGapMetres=0;
  int segments=0;
};
}
