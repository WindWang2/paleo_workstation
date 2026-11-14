// 层：数据
#pragma once
#include "services/pythonprocess.h"

namespace paleo::python {
struct InputBundle;
inline constexpr char SandboxJsonInputProtocol[]="paleo.python-json.v1";
inline constexpr char SandboxAnalysisInputProtocol[]="paleo.python-analysis-frame.v1";
// Host-owned immutable snapshots. Paths, code/argv and environment overrides are
// deliberately absent from the tool-facing request and frozen here by the host.
struct SandboxRequest {
  std::shared_ptr<const VerifiedRuntime> runtime;
  std::shared_ptr<const InputBundle> input;
  ExecutionPolicy policy;
  std::function<bool()> cancelled;
  std::shared_ptr<ExecutionPermit> permit;
};
struct SandboxResult {
  QString code = "python_sandbox_unavailable";
  QByteArray output,error;
  qint64 cpuMicros=0,peakMemoryBytes=0;
  // The peak is the child OS counter; this never claims a GUI/process-wide RSS.
  bool memoryPeakComplete=false;
  qint64 hostSnapshotReservedBytes=0,childMemoryLimitBytes=0;
  int exitCode=-1;
  QByteArray outputLog; // Private bounded diagnostic prefix; never a numerical result.
  bool ok() const {return code==QLatin1String("ok");}
};
} // namespace paleo::python
