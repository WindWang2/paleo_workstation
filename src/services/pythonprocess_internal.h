// 层：数据
#pragma once
#include "pythonprocess.h"
#include <QList>
#include <QStringList>

namespace paleo::python::process_detail {

class ProcessPermit final {
public:
  ~ProcessPermit();
  ProcessPermit(const ProcessPermit &)=delete;
  ProcessPermit &operator=(const ProcessPermit &)=delete;
private:
  ProcessPermit()=default;
  bool m_owns=false;
  friend std::shared_ptr<ProcessPermit> acquireProcessPermit(const std::function<bool()> &cancelled);
};
// One process-wide slot, shared by trusted and generated adapters. A semaphore
// permits a host to prepare input on one thread and release the claim on another.
std::shared_ptr<ProcessPermit> acquireProcessPermit(const std::function<bool()> &cancelled = {});

// Host-created native launch description shared by the trusted and sandbox adapters.
// Native backends own stdio, process lifetime, resource limits and bounded pumping.
// Security-specific launch preparation stays in its respective sandbox adapter.
struct Launch {
  QString program, workingDirectory;
  QStringList arguments, environment;
  QList<QByteArray> inputChunks;
  ExecutionPolicy policy;
  std::function<bool()> cancelled;
  std::shared_ptr<ProcessPermit> permit;
};

ProcessResult runLinuxTrustedChild(const Launch &request);

} // namespace paleo::python::process_detail
