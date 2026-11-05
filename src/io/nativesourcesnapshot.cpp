// 层：数据
#include "nativesourcesnapshot.h"
#include "sourcefingerprint.h"
#include "catalog/datacatalog.h"
#include <paleo_data/service.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUuid>

namespace paleo::document {
namespace {
QString databasePath(const QString &project,const QVariantMap &binding) {
    CatalogVersion location;location.managed=true;location.path=binding.value("path").toString();
    return DataCatalog::resolvedVersionPath(project,location);
}
QString digest(const QString &path,const std::function<bool()> &cancelled,QString *error) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){if(error)*error=file.errorString();return {};}
    QCryptographicHash hash(QCryptographicHash::Sha256);QByteArray buffer(1024*1024,Qt::Uninitialized);
    for(;;) {
        if(cancelled && cancelled()){if(error)*error=QStringLiteral("已取消");return {};}
        const auto count=file.read(buffer.data(),buffer.size());
        if(count<0){if(error)*error=file.errorString();return {};}if(!count)break;
        hash.addData(QByteArrayView(buffer.constData(),count));
    }
    return QString::fromLatin1(hash.result().toHex());
}
}
NativeSourceSnapshot::~NativeSourceSnapshot() {
    if(m_stage)QFile::setPermissions(m_database,QFile::permissions(m_database)|QFileDevice::WriteOwner);
    // Only this instance's generated UUID directory can be owned, never a recovered/user path.
    if(m_publishedOwned && !m_retained) {
        QFile::setPermissions(m_database,QFile::permissions(m_database)|QFileDevice::WriteOwner);
        QDir(m_finalRoot).removeRecursively();
    }
}
std::shared_ptr<NativeSourceSnapshot> NativeSourceSnapshot::prepare(const NativeSourceRelocationRequest &request,
    QString *error,const std::function<bool()> &cancelled) {
    if(error)error->clear();
    const auto fail=[&](const QString &why)->std::shared_ptr<NativeSourceSnapshot>{if(error)*error=why;return {};};
    const auto &original=request.originalBinding;const auto &record=request.source;
    if(original.value("schema").toInt()!=1 || original.value("sourceId").toLongLong()!=record.ref.sourceId ||
       original.value("sourceSha256").toString()!=record.fingerprint.stored())return fail(QStringLiteral("原生模型身份不符"));
    auto result=std::shared_ptr<NativeSourceSnapshot>(new NativeSourceSnapshot);result->m_project=request.projectDirectory;
    result->m_candidateStamp=sourceFileStamp(request.candidatePath);
    if(result->m_candidateStamp.size<0)return fail(QStringLiteral("新来源文件不存在"));
    const bool recovering=!request.recoveryBinding.isEmpty();
    auto binding=recovering?request.recoveryBinding:original;
    if(binding.value("schema").toInt()!=1 || binding.value("sourceId").toLongLong()!=record.ref.sourceId ||
       binding.value("sourceSha256").toString()!=record.fingerprint.stored())return fail(QStringLiteral("快照来源身份不一致"));
    const auto database=databasePath(request.projectDirectory,binding);
    if(database.isEmpty())return fail(QStringLiteral("原生模型路径不安全"));
    FileStamp stamp;qint64 bytes=0;
    if(!verifySourceFingerprint(database,SourceFingerprint::fromStored(binding.value("sha256").toString()),stamp,bytes,error,cancelled))return {};
    paleo::data::DataService reader;std::string detail;paleo::data::SourceIndexEntry entry;
    if(!reader.openProjectReadOnly(QFileInfo(database).absolutePath().toUtf8().toStdString(),detail) ||
       !reader.readSourceIndexEntry(record.ref.sourceId,entry,detail) || !entry.active ||
       entry.sha256!=record.fingerprint.stored().toStdString())return fail(detail.empty()?QStringLiteral("原生来源登记身份不符"):QString::fromUtf8(detail));
    if(recovering) {
        if(!verifySourceFingerprint(request.candidatePath,record.fingerprint,result->m_candidateStamp,
                                    result->m_verifiedBytes,error,cancelled))return {};
        std::string path;
        if(binding.value("storeIdentity").toString()!=record.ref.modelStoreId ||
           !reader.sourcePath(record.ref.sourceId,path,detail) || QDir::cleanPath(QString::fromUtf8(path))!=QDir::cleanPath(request.candidatePath))
            return fail(QStringLiteral("恢复快照与来源操作不一致"));
        result->m_binding=binding;result->m_database=database;result->m_databaseStamp=stamp;
        result->m_finalRoot=QFileInfo(database).absolutePath();result->m_retained=true;return result;
    }
    const auto relativeRoot="artifacts/metadata/native-relocations/"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVariantMap target=binding;target.insert("path",relativeRoot+"/data.sqlite");
    const auto finalDatabase=databasePath(request.projectDirectory,target);
    if(finalDatabase.isEmpty())return fail(QStringLiteral("新原生模型路径不安全"));
    result->m_finalRoot=QFileInfo(finalDatabase).absolutePath();const auto parent=QFileInfo(result->m_finalRoot).absolutePath();
    if(!QDir().mkpath(parent))return fail(QStringLiteral("无法准备原生模型快照目录"));
    result->m_stage=std::make_unique<QTemporaryDir>(QDir(parent).filePath(".stage-XXXXXX"));
    if(!result->m_stage->isValid())return fail(QStringLiteral("无法创建原生模型暂存目录"));
    result->m_database=QDir(result->m_stage->path()).filePath("data.sqlite");
    if(!reader.snapshotDatabase(result->m_database.toUtf8().toStdString(),detail))return fail(QString::fromUtf8(detail));
    if(stamp!=sourceFileStamp(database))return fail(QStringLiteral("原生模型在准备快照时发生变化"));
    reader.closeProject();
    if(cancelled && cancelled())return fail(QStringLiteral("已取消"));
    paleo::data::DataService clone;std::shared_ptr<const paleo::data::PreparedSourceRelocation> prepared;
    if(!clone.openProjectReadOnly(result->m_stage->path().toUtf8().toStdString(),detail) ||
       !clone.prepareSourceRelocation(record.ref.sourceId,request.candidatePath.toUtf8().toStdString(),prepared,detail,cancelled))
        return fail(QString::fromUtf8(detail));
    result->m_verifiedBytes=prepared->verifiedBytes();clone.closeProject();
    if(!clone.openProjectForSourceRelocation(*prepared,detail) || !clone.commitSourceRelocation(*prepared,detail))
        return fail(QString::fromUtf8(detail));
    clone.closeProject();
    const auto hash=digest(result->m_database,cancelled,error);if(hash.isEmpty())return {};
    target.insert("sha256",hash);target.insert("storeIdentity",record.ref.modelStoreId);
    result->m_binding=target;result->m_databaseStamp=sourceFileStamp(result->m_database);
    if(result->m_candidateStamp!=sourceFileStamp(request.candidatePath))return fail(QStringLiteral("新来源在准备快照时发生变化"));
    if(!QFile::setPermissions(result->m_database,QFileDevice::ReadOwner|QFileDevice::ReadUser|QFileDevice::ReadGroup|QFileDevice::ReadOther))
        return fail(QStringLiteral("无法冻结原生模型快照"));
    result->m_databaseStamp=sourceFileStamp(result->m_database);return result;
}
bool NativeSourceSnapshot::publish(QString *error) {
    const auto fail=[&](const QString &why){if(error)*error=why;return false;};
    if(m_databaseStamp!=sourceFileStamp(m_database))return fail(QStringLiteral("原生模型快照准备后已变化"));
    if(!m_stage)return true;
    if(databasePath(m_project,m_binding)!=QDir(m_finalRoot).filePath("data.sqlite"))return fail(QStringLiteral("原生模型发布路径变化"));
    if(!QDir().rename(m_stage->path(),m_finalRoot))return fail(QStringLiteral("原生模型快照发布失败"));
    m_stage->setAutoRemove(false);m_stage.reset();m_database=QDir(m_finalRoot).filePath("data.sqlite");m_publishedOwned=true;return true;
}
}
