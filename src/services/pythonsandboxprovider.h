// 层：数据
#pragma once
#include "pythonsandbox.h"
#include <QJsonObject>
#include <QMutex>

namespace paleo::python {
// inspect is worker-only; snapshots never wait behind a platform probe or permit.
class SandboxProvider {
public:
  explicit SandboxProvider(std::shared_ptr<RuntimeProvider> runtime);
  QJsonObject inspect(const std::function<bool()> &cancelled={});
  QJsonObject snapshot() const;
  SandboxResult run(const SandboxRequest &request) const;
private:
  const std::shared_ptr<RuntimeProvider> m_runtime;
  mutable QMutex m_mutex;
  QJsonObject m_state;
};
}
