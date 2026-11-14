// 层：数据
#pragma once
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <functional>
#include <memory>

namespace paleo::python {
inline constexpr char RuntimeSchema[] = "paleo.python-runtime.v1";
inline constexpr char ProfileId[] = "analysis-core";
inline constexpr char PythonVersion[] = "3.14.10";
inline constexpr char NumpyVersion[] = "2.5.4";

// The application owns this configuration. Tool arguments cannot grant it.
struct RuntimeConfig {
  QString root, manifestSha256;
  bool enabled = false;
};
struct ExecutionPolicy {
  QString id = "analysis-core.v1";
  qint64 codeBytes = 256 * 1024, inputBytes = 256 * 1024 * 1024;
  qint64 outputBytes = 64 * 1024 * 1024, logBytes = 1024 * 1024;
  qint64 diskBytes = 512 * 1024 * 1024, memoryBytes = 512 * 1024 * 1024;
  int cpuSeconds = 60, wallSeconds = 120, threads = 1, concurrent = 1;
  bool allowTrusted = false, allowGenerated = false;
  QJsonObject json() const;
  QString validate() const;
};
struct VerifiedRuntime {
  QString root, interpreter, manifestSha256, runtimeVersion, platform;
  qint64 treeSize = 0;
  // Outside the immutable generation; future GC must also honor persistent operation roots.
  std::shared_ptr<void> retention;
};
struct RuntimeResult {
  QString code = "python_environment_missing";
  std::shared_ptr<const VerifiedRuntime> runtime;
  bool ready() const { return code == QLatin1String("ok") && bool(runtime); }
  QJsonObject json() const;
};
QString runtimePlatform();
QString runtimeSourceLockSha256();
RuntimeResult verifyRuntime(const RuntimeConfig &config, const std::function<bool()> &cancelled = {},
                            std::shared_ptr<void> retention = {});
QString probeRuntime(const VerifiedRuntime &runtime, const std::function<bool()> &cancelled = {});

// Synchronous data service: callers run inspect() through the existing task pool.
// No user code, PATH discovery, downloads, package installation or global state.
class RuntimeProvider {
public:
  explicit RuntimeProvider(RuntimeConfig config, ExecutionPolicy policy = {});
  RuntimeResult inspect(const std::function<bool()> &cancelled = {});
  RuntimeResult snapshot() const;
  ExecutionPolicy policy() const { return m_policy; }
private:
  RuntimeConfig m_config;
  ExecutionPolicy m_policy;
  mutable QMutex m_mutex;
  RuntimeResult m_last;
  std::weak_ptr<void> m_retention;
};
} // namespace paleo::python
