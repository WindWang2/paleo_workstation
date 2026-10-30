// 层：数据
#pragma once
#include "projectdata.h"

// 工区模型分层读取结果（issue #826）。调用方必须区分「工区没有模型分层」与
// 「模型存在但读不出」：后者若回退更早修订或旧 tops 文件，编图/厚度/井预测
// 会静默使用过期分层，且不同页面拿到不同代数据。
enum class ModelTopsStatus
{
  NoModel, // 无工程/无模型库/无解释方案：可按既有规则回退文件 tops
  Ready,   // 读到权威解释；tops 可能为空（该井在模型里没有可用分层）
  Failed   // 打开或读取失败：不得回退，必须把 error 报给用户
};

struct ModelTopsResult
{
  ModelTopsStatus status = ModelTopsStatus::NoModel;
  QVector<WellTop> tops;
  QString error;
};

// 以最新修订为准读取一口井的模型分层。任一修订解码失败即 Failed（不跳过损坏
// 修订去用更早版本）；最新提到该井的修订就是权威来源，其分层为空时不向更早
// 修订借数据。
ModelTopsResult readStratigraphyModelTops(const QString &project, qlonglong wellId);

// 只要「有没有分层」的旧入口：NoModel/Ready 空/Failed 都返回空。需要区分失败
// 的调用方用 readStratigraphyModelTops。
QVector<WellTop> stratigraphyModelTops(const QString &project, qlonglong wellId);
