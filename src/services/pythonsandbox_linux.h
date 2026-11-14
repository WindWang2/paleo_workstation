// 层：数据
#pragma once
#include "services/pythonsandbox.h"
namespace paleo::python {
SandboxResult runLinuxSandbox(const SandboxRequest &request);
QString linuxSandboxReadiness();
}
