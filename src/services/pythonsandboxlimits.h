// 层：数据
#pragma once
#include "pythonsandbox.h"
namespace paleo::python {
inline QString constrainSandboxOutput(const SandboxRequest &original, SandboxRequest &effective) {
  if (!original.policy.validate().isEmpty()) return "python_policy_invalid";
  if (original.stdoutLimitBytes < 0 || original.stdoutLimitBytes > SandboxMaxTypedOutputBytes ||
      original.stdoutLimitBytes > original.policy.outputBytes) return "python_output_limit_invalid";
  effective = original;
  if (original.stdoutLimitBytes) effective.policy.outputBytes = original.stdoutLimitBytes;
  return {};
}
}
