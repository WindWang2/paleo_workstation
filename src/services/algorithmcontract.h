// 层：数据
// services/algorithmcontract.h — 按 AlgorithmSpec 校验并填有效参数。不另存一份默认值。
#pragma once

#include "algorithmspec.h"

#include <QString>
#include <QVariantMap>
#include <QVector>

struct ContractIssue {
  QString code;
  QString field;
  QString message;
};

struct ValidatedParams {
  bool ok = false;
  QString code;
  QVariantMap effective;
  QVector<ContractIssue> issues;
};

// requestedVersion 与登记版本不一致时拒绝，不改叫最新版。
ValidatedParams validateAndFillParams(const QString &algorithmId, int requestedVersion,
                                      const QVariantMap &params);
// 端口边的静态兼容性；目标明确要求的语义不能由未知来源满足。
ContractIssue checkPortCompatibility(const AlgoPort &source, const AlgoPort &target);
