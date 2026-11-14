// 层：数据
#include "pythonsandbox_linux.h"
namespace paleo::python {
QString linuxSandboxReadiness() {
#ifdef Q_OS_LINUX
  return "python_sandbox_qt_unsupported";
#else
  return "python_sandbox_platform_unavailable";
#endif
}
SandboxResult runLinuxSandbox(const SandboxRequest &) {
  SandboxResult result;result.code=linuxSandboxReadiness();return result;
}
} // namespace paleo::python
