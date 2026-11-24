// 层：数据
// services/unitonnxcapabilityspec.h — I2：地层单元 RMS → 本地 ONNX 推理原子。
// 单一身份来源：capabilityId / 工具别名 / 契约版本 / 算法规格 id 只在这里定义，
// 工具表、适配器与装配根都引用本头（不复制第二份字面量）。
#pragma once
#include "capabilityspec.h"
#include <QJsonObject>
#include <QString>

namespace paleo {

// 能力身份（v1）：明确分析单元（S1–S4 子层段）的层段窗口 RMS → 钉权重本地
// ONNX 分类推理。不是通用张量推理平台；不支持任意模型形状/多特征/预处理脚本。
inline constexpr char kUnitOnnxCapabilityId[] = "unit.onnx_predict";
inline constexpr int kUnitOnnxContractVersion = 1;
inline constexpr char kUnitOnnxToolName[] = "paleo.unit_onnx_predict";
inline constexpr char kUnitOnnxAlgorithmId[] = "onnx:unit_rms_predict";
inline constexpr char kUnitOnnxImplementationVersion[] = "1";

// v1 固定的输入网格：prepareSeismicUnitInput 的当前合同（64×64，读取窗口
// maxValues=1,048,576，NaN=缺覆盖）。新原子不放大这些预算。
inline constexpr int kUnitOnnxGridRows = 64;
inline constexpr int kUnitOnnxGridColumns = 64;

// 本输入的规范化特征契约（version 1 词表，经 normalizeRasterFeatureContract）：
// · kind  = layer_window_rms —— 数值来自现有单元层段窗口 RMS 生成过程
//   （readSeismicAnalysisWindow：TWT 层位夹持、重采样 16 样、逐道 RMS、
//   64×64 最近道落格），不是任意振幅栅格；
// · unit  = amplitude —— 原始地震振幅无量纲约定，无单位换算；
// · domain = TWT —— 窗口定义在 TWT(ms) 层位之间；
// · preprocessing = raw_float32_v1 —— float32 原值；缺覆盖格喂 0 且输出端
//   重施有效 mask（不得宣称缺测格获得可信预测）。
// 模型 manifest 的 features 必须声明与之一致的单特征才被接受；按模型名或
// dataType 猜语义一律拒绝。
QJsonObject unitOnnxInputFeatureContract();

// CapabilityRegistry::table() 的单项工厂（Callable；compute 不暗中发布）。
CapabilitySpec unitOnnxCapabilitySpec();

} // namespace paleo
