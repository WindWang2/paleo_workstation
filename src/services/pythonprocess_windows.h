// 层：数据
#pragma once
#include "pythonprocess_internal.h"
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <QVector>
#endif

namespace paleo::python::windows_process {
#ifdef Q_OS_WIN
// Host-only launch policy. A sandbox adapter must supply a complete, valid
// package and capability set; a missing policy is the trusted-template mode.
struct Security {
  PSID packageSid = nullptr;
  QVector<SID_AND_ATTRIBUTES> capabilities;
  bool lessPrivileged = true;
};
#else
struct Security {};
#endif
ProcessResult runChild(const process_detail::Launch &request,const Security *security = nullptr);
} // namespace paleo::python::windows_process
