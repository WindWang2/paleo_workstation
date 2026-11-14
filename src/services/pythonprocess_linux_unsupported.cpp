// 层：数据
#include "pythonprocess_internal.h"
namespace paleo::python::process_detail {
ProcessResult runLinuxTrustedChild(const Launch &) {
  ProcessResult result;result.code="python_process_qt_unsupported";return result;
}
}
