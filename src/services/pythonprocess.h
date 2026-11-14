// 层：数据
#pragma once
#include "pythonruntime.h"
#include <QByteArray>

namespace paleo::python {
namespace process_detail {class ProcessPermit;}
using ExecutionPermit=process_detail::ProcessPermit;
// Claim before materializing large inputs; carry the same opaque ticket to the worker.
std::shared_ptr<ExecutionPermit> claimExecutionPermit(const std::function<bool()> &cancelled = {});

// The host resolves this request from a registered template and a frozen operation.
// This is not a public tool accepting model-supplied code, paths or permissions.
struct TrustedProcessRequest {
  std::shared_ptr<const VerifiedRuntime> runtime;
  QString scriptPath, scriptSha256;
  QByteArray input;
  ExecutionPolicy policy;
  std::function<bool()> cancelled;
  std::shared_ptr<ExecutionPermit> permit;
};

struct ProcessResult {
  QString code = "python_process_unavailable";
  QByteArray output, error;
  qint64 cpuMicros = 0, peakMemoryBytes = 0;
  // Linux reports the greatest live kernel observation; Windows can retain a
  // complete Job peak after exit. This flag distinguishes those two facts.
  bool memoryPeakComplete = false;
  int exitCode = -1;
  QByteArray outputLog; // Bounded diagnostic prefix on failure; never a numerical result.
  bool ok() const { return code == QLatin1String("ok"); }
};

// Synchronous worker-pool primitive. No GUI objects, additional task queue,
// PATH discovery, environment installation or Catalog writes are performed here.
ProcessResult runTrustedProcess(const TrustedProcessRequest &request);

} // namespace paleo::python
