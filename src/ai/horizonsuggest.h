// 层：功能
#pragma once
#include <QString>
#include <QVector>
#include <functional>

class PaleoOnnxService;

// ai/ — 层位自动追踪建议引擎（goal/ai-geological-assist 范围4）。
// 语义：输出「建议」——解释员逐条接受/否决后才可能落库，引擎与调用方都
// 不自动写解释结果（产品级红线）。
//
// 算法（确定性）：种子点（井位/人工拾取）出发，按 (距种子距离, inline,
// xline) 排序逐道扩张；每道以最近已拾取邻道的样本位置为中心取
// [center-window/2, center+window/2] 窗，喂 trace scorer 模型
// （输入 [1,1,T]，输出逐样本概率 [1,1,T]），窗内概率峰为该道建议拾取。
// 置信度 = 峰值处 Bernoulli 熵归一（1−H/ln2）：峰概率 ≈1 → 高置信，
// ≈0.5（背景淹没）→ 低置信。死道（窗全 NaN）跳过——诚实留空，不硬猜。

struct TrackingSeed
{
  int inlineNo = 0;
  int xlineNo = 0;
  int sampleIndex = 0;
};

struct TrackingSuggestion
{
  int inlineNo = 0;
  int xlineNo = 0;
  int sampleIndex = 0; // 建议拾取（绝对样本号）
  float score = 0.0f;      // 峰处模型概率 ∈ (0,1)
  float confidence = 0.0f; // 峰处 Bernoulli 熵归一 ∈ [0,1]
  bool isSeed = false;     // 种子行（解释员给定的锚，不经模型）
  int sourceInline = 0;    // 建议所参考的邻道（追溯链）
  int sourceXline = 0;
};

// 道窗取数：给定 (inline, xline) 与 [sampleBegin, sampleBegin+sampleCount)，
// 填 out（NaN=缺失样本）。返回 false + err → 引擎如实中止。
using TraceWindowFetcher = std::function<bool( int inlineNo, int xlineNo, int sampleBegin,
                                               int sampleCount, QVector<float> &out,
                                               QString &err )>;

// radius：每颗种子向 inline/xline 双向扩张的道数半径；windowSamples：取窗
// 样本数（偶数时中心偏 sampleBegin+window/2）。out 含种子行（isSeed=true）
// 与全部扩张建议；失败 → false + error（out 内容未定义，调用方弃用）。
bool suggestHorizonTracking( PaleoOnnxService *onnx, const QString &model,
                             const QVector<TrackingSeed> &seeds, int windowSamples,
                             int radius, const TraceWindowFetcher &fetch,
                             QVector<TrackingSuggestion> *out, QString *error );
