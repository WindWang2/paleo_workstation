// 层：数据
#include "pythoninputbundle.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <cmath>

namespace paleo::python {
namespace {
QString digest(QByteArrayView bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
bool regular(const QFileInfo &file) {
#ifdef Q_OS_WIN
  if(file.isJunction())return false;
#endif
  return file.isFile() && !file.isSymLink();
}
QString privateRoot(const QString &project,bool create) {
  const auto root=QFileInfo(project).canonicalFilePath();if(root.isEmpty() || !QFileInfo(root).isDir())return {};
  const auto relative=QStringLiteral(".paleo/capability-ops/inputs");
  QString current=root;
  for(const auto &part:relative.split('/')) {
    current+='/'+part;QFileInfo info(current);
#ifdef Q_OS_WIN
    if(info.isJunction())return {};
#endif
    if(info.isSymLink() || (info.exists() && !info.isDir()))return {};
    if((create && !QDir().mkpath(current)) || !QFileInfo(current).isDir() || QFileInfo(current).canonicalFilePath()!=current)return {};
  }
  return current;
}
bool nameAllowed(const QString &name) {
  static const QRegularExpression safe("^[a-z][a-z0-9_.-]{0,63}$");
  static const QRegularExpression device("^(con|prn|aux|nul|com[1-9]|lpt[1-9])$");
  return safe.match(name).hasMatch() && name!="manifest.json" && !name.endsWith('.') && !device.match(name.section('.',0,0)).hasMatch();
}
bool write(const QString &path,QByteArrayView bytes) {
  QSaveFile file(path);return file.open(QIODevice::WriteOnly) && file.write(bytes.data(),bytes.size())==bytes.size() && file.commit();
}
QString verifyFiles(const QString &root,const QJsonObject &manifest,const ExecutionPolicy &policy,const std::function<bool()> &cancelled) {
  if(manifest.value("schema")!="paleo.python-inputs.v1" || !manifest.value("files").isArray() ||
      !manifest.value("metadata").isObject())return "python_input_manifest_invalid";
  const auto files=manifest.value("files").toArray();if(files.size()<2 || files.size()>16)return "python_input_manifest_invalid";
  QSet<QString> names;qint64 total=0;
  for(const auto &item:files) {
    if(cancelled && cancelled())return "cancelled";
    const auto row=item.toObject();const auto name=row.value("path").toString();
    const auto number=row.value("size").toDouble(-1);const auto maximum=name=="code.py"?policy.codeBytes:policy.inputBytes;
    if(!nameAllowed(name) || names.contains(name) || !std::isfinite(number) || number<0 || number>maximum || number!=std::floor(number))return "python_input_manifest_invalid";
    const auto size=qint64(number);if(total>policy.inputBytes+policy.codeBytes-size)return "python_input_budget_exceeded";
    total+=size;names.insert(name);QFile file(QDir(root).filePath(name));
    if(!regular(QFileInfo(file)) || !file.open(QIODevice::ReadOnly) || file.size()!=size)return "python_input_changed";
    QCryptographicHash hash(QCryptographicHash::Sha256);qint64 remaining=size;
    while(remaining>0) {
      if(cancelled && cancelled())return "cancelled";
      const auto bytes=file.read(qMin<qint64>(65536,remaining));if(bytes.isEmpty())return "python_input_changed";
      hash.addData(bytes);remaining-=bytes.size();
    }
    if(file.size()!=size || QString::fromLatin1(hash.result().toHex())!=row.value("sha256").toString())return "python_input_changed";
  }
  if(!names.contains("code.py") || !names.contains("request.json"))return "python_input_manifest_invalid";
  const auto entries=QDir(root).entryInfoList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System);
  if(entries.size()!=names.size()+1)return "python_input_changed";
  for(const auto &info:entries)if(!regular(info) || (info.fileName()!="manifest.json" && !names.contains(info.fileName())))return "python_input_changed";
  return {};
}
}
InputBundle verifyInputBundle(const QString &project,const QJsonObject &reference,const ExecutionPolicy &policy,
                              const std::function<bool()> &cancelled) {
  InputBundle out;out.reference=reference;
  const auto fail=[&](const QString &code){out.code=code;return out;};
  if(!policy.validate().isEmpty())return fail("python_policy_invalid");
  const auto directory=reference.value("directory").toString();const auto sha=reference.value("manifestHash").toString();
  static const QRegularExpression hash("^[0-9a-f]{64}$");
  if(reference.keys()!=QStringList{"directory","manifestHash"} || !hash.match(sha).hasMatch() ||
      directory!=".paleo/capability-ops/inputs/"+sha)return fail("python_input_reference_invalid");
  const auto parent=privateRoot(project,false);if(parent.isEmpty())return fail("python_input_path_unsafe");
  out.root=QDir(parent).filePath(sha);const QFileInfo root(out.root);
  if(root.isSymLink() || !root.isDir() || root.canonicalFilePath()!=out.root)return fail("python_input_path_unsafe");
  QFile file(out.root+"/manifest.json");
  if(!regular(QFileInfo(file)) || !file.open(QIODevice::ReadOnly) || file.size()>64*1024)return fail("python_input_manifest_invalid");
  const auto bytes=file.read(64*1024+1);
  if(bytes.size()>64*1024 || file.size()!=bytes.size() || digest(bytes)!=sha)return fail("python_input_changed");
  const auto document=QJsonDocument::fromJson(bytes);if(!document.isObject())return fail("python_input_manifest_invalid");
  out.manifest=document.object();const auto code=verifyFiles(out.root,out.manifest,policy,cancelled);
  return fail(code.isEmpty()?QStringLiteral("ok"):code);
}
InputBundle freezeInputBundle(const QString &project,const QMap<QString,QByteArray> &files,const QJsonObject &metadata,
                              const ExecutionPolicy &policy,const std::function<bool()> &cancelled) {
  InputBundle out;const auto fail=[&](const QString &code){out.code=code;return out;};
  if(!policy.validate().isEmpty())return fail("python_policy_invalid");
  if(files.size()<2 || files.size()>16 || !files.contains("code.py") || !files.contains("request.json"))return fail("python_input_manifest_invalid");
  QJsonArray entries;qint64 total=0;
  for(auto it=files.cbegin();it!=files.cend();++it) {
    const auto maximum=it.key()=="code.py"?policy.codeBytes:policy.inputBytes;
    if(!nameAllowed(it.key()))return fail("python_input_path_unsafe");
    if(it.value().size()>maximum || total>policy.inputBytes+policy.codeBytes-it.value().size())return fail("python_input_budget_exceeded");
    total+=it.value().size();entries.append(QJsonObject{{"path",it.key()},{"size",double(it.value().size())},{"sha256",digest(it.value())}});
  }
  const QJsonObject manifest{{"schema","paleo.python-inputs.v1"},{"metadata",metadata},{"files",entries}};
  const auto bytes=QJsonDocument(manifest).toJson(QJsonDocument::Compact);if(bytes.size()>64*1024)return fail("python_input_manifest_invalid");
  const auto sha=digest(bytes);const auto parent=privateRoot(project,true);if(parent.isEmpty())return fail("python_input_path_unsafe");
  out.reference={{"directory",".paleo/capability-ops/inputs/"+sha},{"manifestHash",sha}};
  if(QFileInfo::exists(parent+'/'+sha))return verifyInputBundle(project,out.reference,policy,cancelled);
  QTemporaryDir temporary(parent+"/.prepare-XXXXXX");if(!temporary.isValid())return fail("python_input_write_failed");
  for(auto it=files.cbegin();it!=files.cend();++it) {
    if(cancelled && cancelled())return fail("cancelled");
    if(!write(temporary.path()+'/'+it.key(),it.value()))return fail("python_input_write_failed");
  }
  if(!write(temporary.path()+"/manifest.json",bytes))return fail("python_input_write_failed");
  if(!QDir().rename(temporary.path(),parent+'/'+sha) && !QFileInfo::exists(parent+'/'+sha))return fail("python_input_write_failed");
  return verifyInputBundle(project,out.reference,policy,cancelled);
}
} // namespace paleo::python
