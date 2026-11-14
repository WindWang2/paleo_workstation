// 层：功能
#include "capabilityrecovery_internal.h"
#include "services/capabilityspec.h"
#include <cmath>

namespace paleo::capability::recovery_detail {
unsigned operationEffects(const OperationLog::Record &record) {
  const auto declared=record.frozenRequest.value("executionEffects");
  unsigned extra=0;
  if(!declared.isUndefined()) {
    const auto value=declared.toDouble(-1);
    if(!declared.isDouble() || !std::isfinite(value) || value<0 || value>127 || value!=std::floor(value))return 0;
    extra=unsigned(value);
  }
  const auto phase=host::computeEffects(record.frozenRequest.value("phase").toString());
  return phase?phase|extra|CapabilityRegistry::executionEffects(record.capabilityId,record.contractVersion):0;
}
} // namespace paleo::capability::recovery_detail
