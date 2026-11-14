// 层：数据
#pragma once
#include "pythonsandbox.h"

namespace paleo::python {
SandboxResult runWindowsSandbox(const SandboxRequest &request);
// A read-only platform probe; runtime/input/permissions are rechecked at launch.
QJsonObject windowsSandboxReadiness();
} // namespace paleo::python
