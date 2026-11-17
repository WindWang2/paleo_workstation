// 层：数据
#pragma once
#include "piruntime.h"
#include <QByteArray>

namespace paleo::pi {
struct HandshakeRequest {
  bool enabled=false; // Re-check the host opt-in at launch; a retained identity is not an enable grant.
  std::shared_ptr<const VerifiedRuntime> runtime;
  QString sessionId,generation; // Host-owned opaque session + lossless decimal epoch.
  std::function<bool()> cancelled;
};
struct HandshakeResult {
  QString code;
  QJsonObject peer;
  QByteArray diagnostics;
  bool ok()const{return code==QLatin1String("ok");}
};
// Fixed short-lived hello/close exchange on a worker; no prompts, tools, keys or general RPC.
// This trusted sidecar is not a sandbox. A1 does not claim OS memory/CPU hard limits.
HandshakeResult runHandshake(const HandshakeRequest&);
}
