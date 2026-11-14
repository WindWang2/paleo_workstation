// 层：数据
#pragma once
#include "pythonsandbox.h"
#include <QJsonObject>
#include <QMutex>

namespace paleo::python {
// inspect is worker-only; snapshots never wait behind a platform probe or permit.
class SandboxProvider {
public:
  QJsonObject inspect(const std::function<bool()> &cancelled={});
  QJsonObject snapshot() const;
  SandboxResult run(const SandboxRequest &request) const;
private:
  mutable QMutex m_mutex;
  QJsonObject m_state;
};
}
