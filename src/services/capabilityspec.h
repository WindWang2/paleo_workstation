// 层：数据
// services/capabilityspec.h — 业务能力目录。参数默认值只存在于 AlgorithmSpec。
#pragma once

#include "algorithmspec.h"

#include <QString>
#include <QStringList>
#include <QVector>

enum class CapabilityKind { Query, Command, Compute, Workflow };

enum class CapabilityExposure { Internal, LegacyAdapter, Callable, Unavailable };

struct CapabilitySpec {
  QString capabilityId;
  int contractVersion = 1;
  CapabilityKind kind = CapabilityKind::Compute;
  QString algorithmId;
  int algorithmContractVersion = 1;
  CapabilityExposure exposure = CapabilityExposure::Internal;
  QString exposureReason;
  QStringList aliases;
  QString publishEffect; // none | stage | publish
  bool cancelSupported = false;
  bool resumeSupported = false; // 无检查点。可从冻结计划重跑，不算恢复。
  bool restartable = true;
  QStringList operatorShapes;
};

class CapabilityRegistry {
public:
  static QVector<CapabilitySpec> all();
  static CapabilitySpec spec(const QString &capabilityId, int contractVersion);
  static QString resolveAlias(const QString &name, QString *code);
  static QStringList aliasCollisions();
  static CapabilityExposure exposureForAlgorithm(const QString &algorithmId, QString *reason);
  static QStringList callableIds();
};

QString capabilityExposureName(CapabilityExposure exposure);
QString executionShapeName(AlgorithmExecutionShape shape);
