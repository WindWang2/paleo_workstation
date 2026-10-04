// 层：数据
#pragma once

#include "candidateboundaries.h"
#include "dominantfacies.h"

#include <QVariantMap>

#include <map>
#include <string>
#include <vector>

// faciesmapping/evidencesynthesis — 多源证据合成（goal/facies-automapping
// 阶段3 纯计算核）。候选编图单元（阶段2）× 多来源点证据（井相/单因素图/
// 预测相/专家约束）加权投票 → 单元相归属 + 置信度。诚实语义：票数不足或
// 加权得分份额低于阈值 → 不赋相（assignedCode = -1）；无点证据但有约束相
// 代码 → 继承约束并如实标记（confidence = 0，证据来源=约束而非井）。

namespace paleo::faciesmapping
{

struct EvidenceSample
{
  double x = 0;
  double y = 0;
  int faciesCode = -1;     // <0 的样本不计票
  double confidence = 1.0; // ∈ [0,1]，与源权重相乘
  double weightBoost = 1.0; // 样本级加成（如优势相的 dominance）；<=0 视为 1
};

struct EvidenceSource
{
  std::string id;   // "well_facies" / "factor:<name>" / "remote_prediction" / …
  std::string kind; // "well"|"factor"|"prediction"|"constraint"（诊断用）
  double weight = 1.0;
  std::vector<EvidenceSample> samples;
};

struct SynthesisOptions
{
  double assignThreshold = 0.5; // 最高得分份额 < 阈值 → 不赋相
  int minVotes = 1;             // 单元内有效票数下限
};

struct RegionSynthesis
{
  std::string regionId;
  int assignedCode = -1;
  double confidence = 0;  // 最高得分 / 总得分 ∈ [0,1]；约束继承时 = 0
  int voteCount = 0;
  bool constraintInherited = false; // 无点票，相来自候选单元约束代码
  std::map<int, double> scores;     // faciesCode → 加权得分（降序展开见 scoresRanked）
  std::vector<std::pair<int, double>> scoresRanked;
  std::vector<std::string> contributingSources;
};

struct SynthesisResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<RegionSynthesis> regions;  // 与输入 regions 同序同长
  QVariantMap diagnostics;
};

// regions 来自 extractCandidateRegions（geometry + regionId + faciesCode 作
// 约束继承候选）。sources 为空且全部单元无约束代码 → Ok + 全 -1（如实）。
SynthesisResult synthesizeFaciesRegions( const std::vector<CandidateRegion> &regions,
                                         const std::vector<EvidenceSource> &sources,
                                         const SynthesisOptions &options = {},
                                         const Control *control = nullptr );

} // namespace paleo::faciesmapping
