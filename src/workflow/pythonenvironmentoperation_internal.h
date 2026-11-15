// 层：功能
#pragma once
#include "pythonenvironmentoperation.h"
namespace paleo::python {
struct EnvironmentOperationContract {
  bool allowLegacyPlans=false; // Only contracts actually issued before E2 opt in.
  std::function<capability::CapabilityExecutionAdapter(const EnvironmentSelection &)> captureAdapter;
  std::function<QString(const capability::OperationLog::Record &,const EnvironmentSelection &)> checkRecord;
  std::function<capability::OperationInputValidator(const QJsonObject &,const EnvironmentSelection &)> inputValidator;
  std::function<capability::OperationWorker(const QJsonObject &,const EnvironmentSelection &)> worker;
};
capability::CapabilityExecutionAdapter environmentOperationAdapter(EnvironmentMode,
  std::shared_ptr<PythonEnvironmentRegistry>,EnvironmentOperationContract);
}
