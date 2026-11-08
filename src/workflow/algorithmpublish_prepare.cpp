// 层：功能
#include "algorithmpublish.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <utility>

namespace paleo {
namespace {
bool filePort(const AlgoPort &port) {
  return port.kind==QLatin1String("raster") || port.kind==QLatin1String("vector") ||
      port.kind==QLatin1String("table") || port.kind==QLatin1String("file");
}
bool copyAndAttest(const QString &source,const QString &destination,DerivedFileFingerprint &fingerprint,
    QString &error,const std::function<bool()> &cancelled,qint64 maxBytes,bool &created) {
  created=false;
  const bool copy=QDir::cleanPath(source)!=QDir::cleanPath(destination);
  const QFileInfo before(source);
  QFile input(source),output(destination);
  if (!input.open(QIODevice::ReadOnly) || (copy && !output.open(QIODevice::WriteOnly|QIODevice::NewOnly))) {
    error=QStringLiteral("无法准备输出副本（目标必须尚不存在）：%1").arg(destination);return false;
  }
  created=copy;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 consumed=0;
  while (!input.atEnd()) {
    if (cancelled && cancelled()) {error=QStringLiteral("输出集准备已取消");return false;}
    const auto bytes=input.read(qMin<qint64>(1024*1024-1,maxBytes-consumed)+1);
    if (bytes.size()>maxBytes-consumed) {error=QStringLiteral("输出文件增长超过准备预算");return false;}
    if (bytes.isEmpty() || (copy && output.write(bytes)!=bytes.size())) {error=QStringLiteral("输出集复制失败");return false;}
    consumed+=bytes.size();
    hash.addData(bytes);
  }
  if (copy && !output.flush()) {error=QStringLiteral("输出集落盘失败");return false;}
  if (copy) output.close();
  input.close();
  const QFileInfo after(source),stored(destination);
  if (before.size()!=after.size() || before.lastModified()!=after.lastModified() || stored.size()!=before.size()) {
    error=QStringLiteral("算法输出在准备期间已变化");return false;
  }
  fingerprint.path=stored.absoluteFilePath();fingerprint.size=stored.size();fingerprint.modified=stored.lastModified();
  fingerprint.sha256=QString::fromLatin1(hash.result().toHex());
  return fingerprint.matches(destination);
}
bool validateSetPorts(const AlgorithmSpec &spec,const AlgorithmRunResult &result,
    const QVector<AlgorithmOutputTarget> &targets,QString &error) {
  if (!spec.complete || spec.id.isEmpty() || !result.ok || result.artifacts.isEmpty() ||
      result.artifacts.size()>64 || targets.size()!=result.artifacts.size()) {
    error=QStringLiteral("输出集需要完整算法规格、成功结果及对应目标");return false;
  }
  QHash<QString,AlgoPort> ports;
  for (const auto &port : spec.outputs) {
    if (port.id.isEmpty() || ports.contains(port.id)) {error=QStringLiteral("算法端口重复/无效");return false;}
    ports.insert(port.id,port);
  }
  QSet<QString> found,targetPorts,paths;
  for (const auto &target : targets) {
    if (targetPorts.contains(target.portId) || !target.staging.isValid() || paths.contains(target.staging.absolutePath)) {
      error=QStringLiteral("输出目标端口或路径重复/无效");return false;
    }
    targetPorts.insert(target.portId);paths.insert(target.staging.absolutePath);
  }
  for (const auto &artifact : result.artifacts) {
    if (!ports.contains(artifact.portId) || !filePort(ports.value(artifact.portId)) ||
        found.contains(artifact.portId) || !targetPorts.contains(artifact.portId)) {
      error=QStringLiteral("输出端口未知、重复或缺少目标：%1").arg(artifact.portId);return false;
    }
    found.insert(artifact.portId);
  }
  for (const auto &port : spec.outputs)
    if (filePort(port) && port.required && !found.contains(port.id)) {
      error=QStringLiteral("缺少必需输出端口：%1").arg(port.id);return false;
    }
  return true;
}
}

bool prepareAlgorithmOutputSet(const AlgorithmSpec &spec,const AlgorithmRunResult &result,
    const QVector<AlgorithmOutputTarget> &targets,DerivedPreparedSet &prepared,
    QString *error,const std::function<bool()> &cancelled,const OutputSetPreparationBudget &budget) {
  prepared={};QString why;
  const auto fail=[&](const QString &message){if(error)*error=message;prepared={};return false;};
  if (budget.maxTotalBytes<0) return fail(QStringLiteral("输出集预算无效"));
  if (!validateSetPorts(spec,result,targets,why)) return fail(why);
  if (cancelled && cancelled()) return fail(QStringLiteral("输出集准备已取消"));
  QHash<QString,DerivedStaging> byPort;
  for (const auto &target : targets) byPort.insert(target.portId,target.staging);
  qint64 total=0;
  QHash<QString,qint64> sizes;
  for (const auto &artifact : result.artifacts) {
    const QFileInfo file(artifact.absolutePath);
    if (!file.isAbsolute() || !file.isFile() || file.isSymLink() || file.size()<0 ||
        file.size()>budget.maxTotalBytes-total) return fail(QStringLiteral("输出文件无效或超过输出集字节预算"));
    total+=file.size();
    sizes.insert(artifact.portId,file.size());
  }
  DerivedPreparedSet candidate;candidate.algorithmId=spec.id;
  const auto carrier=byPort.contains("OUTPUT")?byPort.value("OUTPUT"):targets.first().staging;
  candidate.id=carrier.versionId; // unique, indexed receipt carrier; not an operation ID
  QStringList createdPaths;
  for (const auto &artifact : result.artifacts) {
    DerivedPreparedMember member;member.portId=artifact.portId;member.staging=byPort.value(artifact.portId);member.extra=artifact.extra;member.parentPortIds=artifact.parentPortIds;
    bool created=false;
    const bool ok=copyAndAttest(artifact.absolutePath,member.staging.absolutePath,member.fingerprint,why,cancelled,
                              sizes.value(artifact.portId),created);
    if (created) createdPaths.append(member.staging.absolutePath);
    if (!ok || (cancelled && cancelled())) {
      for (const auto &path : createdPaths) QFile::remove(path);
      return fail(why.isEmpty()?QStringLiteral("输出集准备已取消或校验失败"):why);
    }
    candidate.members.append(member);
  }
  prepared=std::move(candidate);if(error)error->clear();return true;
}
}
