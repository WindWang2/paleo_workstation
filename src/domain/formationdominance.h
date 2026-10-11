// 层：数据
#pragma once
#include <QString>
#include <QVector>

// 地层优势相（优势微相）纯数据：一个地层内按累计厚度最大的相。
// 例：地层厚 100 m，分流河道累计 60 m，优势微相就是分流河道（占比 0.6）。

// 一段已解释/预测的相观测（MD，m）。code < 0 = 只有文字没有分类编码。
struct FaciesSpan {
  double top = 0, bottom = 0;
  QString label;
  int code = -1;
  double confidence = 1.0; // ∈ [0,1]，只加权优势裁决，不改厚度记账
};

// 一个地层单位（组或段）的优势相统计。dominant 为空 = 该地层没有任何相观测。
struct FormationDominance {
  QString formation, unitType; // 地层名；层级「组」/「段」/空
  double top = 0, bottom = 0;  // 地层 MD 范围
  QString dominant;            // 优势微相文字
  int dominantCode = -1;       // 分类编码（观测带编码时）
  double thickness = 0;        // 地层厚度
  double classifiedThickness = 0; // 已有相观测的厚度（重叠不双算）
  double dominantThickness = 0;   // 优势相累计厚度
  double ratio = 0;            // 优势相厚度 / 已分类厚度
  double coverage = 0;         // 已分类厚度 / 地层厚度
  bool tie = false;            // 优势并列（取更浅出现者，如实标记）
  QString composition;         // 「分流河道 60.00 m；河口坝 40.00 m」
};
