// 层：数据
#pragma once
#include "domain/wellfaciesrequest.h"
#include <functional>

namespace paleo::wellfacies {
// Host-private worker input. It contains values only; never a catalog/session/widget pointer.
struct InputJob {
  QString projectDir;
  Request request;
  WellFaciesModel model;
  QVariantList facies;
  QHash<QString,QVariantList> faciesSchemas;
};
PreparedInput prepareInput(const InputJob &job, const std::function<bool()> &cancelled = {});
struct InputInspectionJob {
  QString projectDir;
  document::WellRef well;
  std::optional<document::InterpretationContext> context;
};
QJsonObject inspectInput(const InputInspectionJob &job,const std::function<bool()> &cancelled = {});
} // namespace paleo::wellfacies
