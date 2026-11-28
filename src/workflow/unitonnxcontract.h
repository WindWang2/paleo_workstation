// 层：功能
#pragma once
#include "domain/stratigraphicunit.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QRectF>
#include <QString>
#include <functional>

class PaleoOnnxService;

namespace paleo {

// unit.onnx_predict v1 的冻结计划。prepare 阶段在任务线程物化并整体进入
// 持久 operation journal；compute 只消费本计划，不重读地震原件——重启后
// 恢复用的也是同一份冻结 64×64 层段 RMS 快照，不是 live 重取的近似值。
// 序列化校验（fromJson）是篡改/损坏闸：schemaHash 覆盖全部数值身份字段。
struct FrozenUnitOnnxPlan {
  QString unitKey;                 // analysisLayerKey(frozen unit)
  QJsonObject unit;                // frozen UnitSelection (toMap；含具体 revision)
  QString seismicVersionId;        // catalog 版本 id（唯一父引用）
  QString seismicSha256;           // catalog 版本 sha256（prepare 时值）
  QString seismicPath;             // prepare 时解析出的绝对路径（诊断/输入校验用）
  qlonglong seismicSize = -1;      // 有界源指纹：文件字节（不整读 100GB 原件）
  qlonglong seismicMtimeMs = -1;   // 有界源指纹：mtime(ms)
  QString model;                   // 注册表模型名
  QString modelSha256;             // 注册表钉 SHA（compute 走 runPinnedTensor）
  QString inputName;               // ORT 实测首输入名
  QString inputSignature;          // OnnxModelMeta.inputSignature（如 x:float32[1,1,?,?]）
  QString outputSemantics;         // logits | probabilities（v1 仅分类）
  int classCount = 0;              // 实测输出通道数（= manifest 声明）
  int featureContractVersion = 0;
  QJsonArray modelFeatures;        // 模型声明特征（规范化前原值，供回查）
  int rows = 64;
  int columns = 64;
  int validCells = 0;
  QRectF extent;                   // 单元窗口二维范围（像元中心外扩半格）
  QString crs;                     // 窗口 CRS（WKT）
  QByteArray samples;              // rows*columns float32 小端原值；NaN=缺覆盖
  QString featureSchemaHash;
  QJsonObject toJson() const;
  static bool fromJson(const QJsonObject &, FrozenUnitOnnxPlan &, QString *error);
  // 输入样本字节摘要（诊断/receipt 用）。
  QString samplesSha256() const;
};

// owner 线程（CapabilityHost::prepare 的 capture 分支）解析好的受信输入值。
// catalog 查询发生在此之前；本结构只携带值，worker 不碰 catalog。
struct UnitOnnxPrepareSource {
  QString projectDir;
  QString model;
  QString seismicVersionId;
  QString seismicSha256;
  QString seismicPath;
  qlonglong seismicSize = -1;
  qlonglong seismicMtimeMs = -1;
  paleo::strat::UnitSelection unit; // 请求单元（本函数内冻结）
};

// prepare 的重型物化（任务线程）：注册表钉身份复核 → 真实 ORT 加载与
// 签名/特征合同核对 → freezeAnalysisUnit → prepareSeismicUnitInput 物化
// 64×64 RMS → 组装冻结计划。任何一步不符都如实失败，不猜语义、不换模型。
bool prepareUnitOnnxPlan(PaleoOnnxService *onnx, const UnitOnnxPrepareSource &source,
                         FrozenUnitOnnxPlan &out, const std::function<bool()> &cancelled,
                         QString *error);

// 层段取数失败句 → 机器码。取消（含「地震层位窗口读取已取消」）不是来源缺失。
QString unitOnnxInputFailureCode(const QString &message);

} // namespace paleo
