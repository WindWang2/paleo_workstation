// 层：数据
#pragma once
#include "pythonruntime.h"

namespace paleo::python {
// Inspect through the existing task pool. Snapshot never waits for an execution
// permit, validates files or starts a process; launch still performs its own checks.
class TrustedReadinessProvider {
public:
  explicit TrustedReadinessProvider(std::shared_ptr<RuntimeProvider> runtime);
  QJsonObject inspect(const std::function<bool()> &cancelled = {});
  QJsonObject snapshot() const;
private:
  std::shared_ptr<RuntimeProvider> m_runtime;
  mutable QMutex m_mutex;
  QJsonObject m_last;
};
} // namespace paleo::python
