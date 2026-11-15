// 层：数据
#include "pythonsandbox_linux.h"
namespace paleo::python {
SandboxResult runLinuxSandbox(const SandboxRequest &) {
  SandboxResult result;
#ifdef Q_OS_LINUX
  result.code="python_sandbox_qt_unsupported";
#else
  result.code="python_sandbox_platform_unavailable";
#endif
  return result;
}
} // namespace paleo::python
