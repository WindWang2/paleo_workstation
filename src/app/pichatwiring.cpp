// 层：组装根
#include "pichatwiring.h"

paleo::pi::RuntimeConfig capturePiChatRuntime() {
  return {qEnvironmentVariable("PALEO_AGENT_PI_ENABLE") == "1", qEnvironmentVariable("PALEO_AGENT_PI_ROOT"),
    qEnvironmentVariable("PALEO_AGENT_PI_MANIFEST_SHA256")};
}
