// 层：数据
#include "pythonsandbox_windows_internal.h"
#ifdef Q_OS_WIN
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace paleo::python::windows_detail {
namespace {
constexpr auto Schema = "paleo.python-sandbox-profile.v1";
QString journalRoot(QString *error) {
  const auto app=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if(app.isEmpty()){*error="profile_journal_location";return {};}
  const auto path=QDir(app).filePath("python-sandbox-control");
  if(!QDir().mkpath(path)){*error="profile_journal_create";return {};}
  const auto root=canonicalRoot(path);
  if(root.isEmpty()||!protectedFileAcl(root,nullptr,error))return {};
  return root;
}
quint64 creation(HANDLE process) {
  FILETIME first{},last{},kernel{},user{};if(!GetProcessTimes(process,&first,&last,&kernel,&user))return 0;
  return (quint64(first.dwHighDateTime)<<32)|first.dwLowDateTime;
}
bool validMoniker(const QString &moniker) {
  static const QRegularExpression pattern("^PaleoPython\\.[0-9a-f]{32}$");return pattern.match(moniker).hasMatch();
}
bool ownerStopped(DWORD id,quint64 started,bool &known) {
  known=true;Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,id));
  if(!process.valid()){if(GetLastError()==ERROR_INVALID_PARAMETER)return true;known=false;return false;}
  const auto actual=creation(process.get());if(!actual){known=false;return false;}
  return actual!=started||WaitForSingleObject(process.get(),0)==WAIT_OBJECT_0;
}
bool removeNamed(const QString &moniker,QString *error) {
  const auto code=api().remove((PCWSTR)moniker.utf16());
  if(SUCCEEDED(code)||code==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)||code==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)||code==HRESULT_FROM_WIN32(ERROR_NOT_FOUND))return true;
  *error="profile_remove:"+QString::number(DWORD(code));return false;
}
} // namespace
bool registerProfile(Profile &profile,QString *error) {
  const auto root=journalRoot(error);if(root.isEmpty())return false;
  QLockFile lock(QDir(root).filePath(".lock"));lock.setStaleLockTime(30000);
  if(!lock.tryLock(5000)){*error="profile_journal_busy";return false;}
  const auto started=creation(GetCurrentProcess());if(!started){*error="profile_owner_identity";return false;}
  profile.journal=QDir(root).filePath(profile.moniker+".json");
  if(QFileInfo::exists(profile.journal)){*error="profile_journal_collision";profile.journal.clear();return false;}
  const QJsonObject record{{"schema",Schema},{"moniker",profile.moniker},{"hostPid",qint64(GetCurrentProcessId())},
      {"hostCreated100ns",QString::number(started)}};
  QSaveFile file(profile.journal);const auto bytes=QJsonDocument(record).toJson(QJsonDocument::Compact);
  if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){*error="profile_journal_write";return false;}
  return protectedFileAcl(profile.journal,nullptr,error);
}
bool Profile::cleanup(QString *error) {
  QString local;if(!error)error=&local;
  std::unique_ptr<QLockFile> lock;
  if(!journal.isEmpty()){lock=std::make_unique<QLockFile>(QDir(QFileInfo(journal).absolutePath()).filePath(".lock"));lock->setStaleLockTime(30000);
    if(!lock->tryLock(5000)){*error="profile_journal_busy";return false;}}
  if(created&&!removeNamed(moniker,error))return false;created=false;
  if(!journal.isEmpty()&&QFileInfo::exists(journal)&&!QFile::remove(journal)){*error="profile_journal_remove";return false;}
  journal.clear();return true;
}
bool cleanDeadProfiles(QString *error) {
  const auto root=journalRoot(error);if(root.isEmpty())return false;
  QLockFile lock(QDir(root).filePath(".lock"));lock.setStaleLockTime(30000);
  if(!lock.tryLock(5000)){*error="profile_journal_busy";return false;}
  const auto files=QDir(root).entryInfoList({"*.json"},QDir::Files|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot,QDir::Name);
  if(files.size()>256){*error="profile_journal_budget";return false;}
  for(const auto &info:files){
    if(info.isSymLink()||info.isJunction()||!info.isFile()||info.size()>4096||info.suffix()!="json"){*error="profile_journal_invalid";return false;}
    QFile file(info.absoluteFilePath());if(!file.open(QIODevice::ReadOnly)){*error="profile_journal_read";return false;}
    QJsonParseError parse;const auto doc=QJsonDocument::fromJson(file.readAll(),&parse);file.close();
    if(parse.error!=QJsonParseError::NoError||!doc.isObject()){*error="profile_journal_invalid";return false;}
    const auto record=doc.object();const auto moniker=record.value("moniker").toString();bool numeric=false;
    const auto started=record.value("hostCreated100ns").toString().toULongLong(&numeric);const auto pid=record.value("hostPid").toInteger(-1);
    if(record.keys()!=QStringList{"hostCreated100ns","hostPid","moniker","schema"}||record.value("schema").toString()!=QLatin1String(Schema)||
        !validMoniker(moniker)||info.fileName()!=moniker+".json"||!numeric||!started||pid<1||pid>0xffffffffLL){*error="profile_journal_identity";return false;}
    bool known=false;if(!ownerStopped(DWORD(pid),started,known)){if(!known){*error="profile_owner_unknown";return false;}continue;}
    if(!removeNamed(moniker,error)||!QFile::remove(info.absoluteFilePath())){if(error->isEmpty())*error="profile_journal_remove";return false;}
  }
  return true;
}
} // namespace paleo::python::windows_detail
#endif
