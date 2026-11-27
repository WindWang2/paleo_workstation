// 层：数据
#pragma once
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

namespace paleo::pi {
struct RuntimeConfig {
  bool enabled=false;
  QString root,manifestSha256; // One complete identity from trusted host configuration.
};
// Locates the pinned package relative to the executable in a build or installed tree.
RuntimeConfig bundledRuntime();
struct RuntimeInspection;
class VerifiedRuntime final {
public:
  const QString &root()const{return m_root;}
  const QString &manifestSha256()const{return m_manifestSha256;}
  const QString &nodeProgram()const{return m_node;}
  const QString &entryPoint()const{return m_entry;}
  const QString &nodeVersion()const{return m_nodeVersion;}
  const QString &platform()const{return m_platform;}
  const QString &upstreamCommit()const{return m_upstream;}
  const QString &coreVersion()const{return m_coreVersion;}
private:
  VerifiedRuntime()=default;
  QString m_root,m_manifestSha256,m_node,m_entry,m_nodeVersion,m_platform,m_upstream,m_coreVersion;
  friend RuntimeInspection inspectRuntime(const RuntimeConfig&,const std::function<bool()>&);
};
struct RuntimeInspection {
  QString code;
  std::shared_ptr<const VerifiedRuntime> runtime;
  QJsonObject status;
  bool ok()const{return code==QLatin1String("ok")&&bool(runtime);}
};
// Worker-only identity verification. Never starts Node or discovers it through PATH.
RuntimeInspection inspectRuntime(const RuntimeConfig&,const std::function<bool()> &cancelled={});
}
