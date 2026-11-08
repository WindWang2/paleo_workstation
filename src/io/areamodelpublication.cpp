// 层：数据
#include "areamodelpublication.h"
#include "areamodel.h"
#include "../catalog/areasourceprojection.h"
#include "../catalog/areawellprojection.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

namespace paleo::area_model {
namespace {
using paleo::data::ModelChangeHead;
QString absolute(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
QString intentPath(const QString &project) { return directory(project) + QStringLiteral(".publication.json"); }
QString text(const std::string &value) { return QString::fromUtf8(value); }
bool fail(QString *error, const QString &message) { if (error) *error=message; return false; }
QJsonObject encode(const ModelChangeHead &head) {
    return {{"store",text(head.storeId)},{"generation",text(head.generation)},
            {"sequence",QString::number(head.sequence)},{"available",head.available}};
}
ModelChangeHead decode(const QJsonObject &value) {
    ModelChangeHead head; head.storeId=value["store"].toString().toStdString();
    head.generation=value["generation"].toString().toStdString();
    bool ok=false; head.sequence=value["sequence"].toString().toLongLong(&ok);
    head.available=value["available"].toBool() && ok && head.sequence>=0 &&
        !head.storeId.empty() && !head.generation.empty();
    return head;
}
bool equal(const ModelChangeHead &a,const ModelChangeHead &b) {
    return a.available && b.available && a.storeId==b.storeId &&
        a.generation==b.generation && a.sequence==b.sequence;
}
bool save(const QString &path,const QJsonObject &value,QString *error) {
    QSaveFile file(path); file.setDirectWriteFallback(false);
    const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())
        return fail(error,QStringLiteral("无法持久保存模型发布记录：")+file.errorString());
    return true;
}
bool load(const QString &path,QJsonObject &value,QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size()>64*1024)
        return fail(error,QStringLiteral("模型发布记录不可读或超过预算"));
    QJsonParseError detail;
    const auto json=QJsonDocument::fromJson(file.readAll(),&detail);
    if (detail.error!=QJsonParseError::NoError || !json.isObject())
        return fail(error,QStringLiteral("模型发布记录损坏"));
    value=json.object(); return true;
}
bool readHead(const QString &root,ModelChangeHead &head,QString *error) {
    paleo::data::DataService reader; std::string detail;
    if (!reader.openProjectReadOnly(root.toUtf8().toStdString(),detail) ||
        !reader.readModelChangeHead(head,detail) || !head.available)
        return fail(error,QStringLiteral("无法核对模型提交基底：")+text(detail));
    return true;
}
bool preservesManagedSources(const QString &active,const QString &prepared,QString *error) {
    paleo::data::DataService reader;std::string detail;
    std::vector<paleo::data::SourceIndexEntry> sources;
    if(!reader.openProjectReadOnly(prepared.toStdString(),detail) || !reader.listSourceIndexEntries(sources,detail))
        return fail(error,text(detail));
    for(const auto &source:sources) {
        const auto path=text(source.storedPath);
        if(path.isEmpty() || QFileInfo(path).isAbsolute())continue;
        if(QFileInfo::exists(QDir(active).filePath(path)) && !QFileInfo::exists(QDir(prepared).filePath(path)))
            return fail(error,QStringLiteral("模型暂存缺少已关联的受管原件，未替换当前模型：")+path);
    }
    return true;
}
bool validate(const QString &project,const QJsonObject &record,QString *error) {
    const auto operation=record["operation"].toString();
    const auto staging=record["staging"].toString();
    const auto expected=decode(record["expected"].toObject()),target=decode(record["target"].toObject());
    if (record["schema"].toInt()!=1 || QUuid(operation).isNull() ||
        record["project"].toString()!=absolute(project) ||
        absolute(QFileInfo(staging).absolutePath())!=absolute(QFileInfo(project).absolutePath()) ||
        !QFileInfo(staging).fileName().startsWith(".paleo-area-") ||
        record["backup"].toString()!=directory(project)+".previous-"+operation ||
        !expected.available || !target.available || expected.storeId!=target.storeId || expected.generation==target.generation)
        return fail(error,QStringLiteral("模型发布记录的格式、归属或路径无效"));
    return true;
}
bool verifyOwner(const QJsonObject &record,QString *error) {
    QJsonObject owner;
    return load(record["staging"].toString()+"/.paleo-area-publication.json",owner,error) &&
        (owner["operation"]==record["operation"] && owner["project"]==record["project"] ? true :
         fail(error,QStringLiteral("模型暂存目录的操作归属不匹配")));
}
bool advance(const QString &project,const QJsonObject &record,QString *error) {
    const auto active=directory(project), backup=record["backup"].toString();
    const auto prepared=directory(record["staging"].toString());
    const auto target=decode(record["target"].toObject());
    ModelChangeHead current;
    if (QFileInfo::exists(active+"/data.sqlite")) {
        if (!readHead(active,current,error)) return false;
        if (current.storeId==target.storeId && current.generation==target.generation &&
            current.sequence>=target.sequence) return true;
        if (!equal(current,decode(record["expected"].toObject())))
            return fail(error,QStringLiteral("ModelHeadConflict：模型已被更新，保留当前模型及待恢复记录"));
        if (QFileInfo::exists(backup))
            return fail(error,QStringLiteral("模型备份目录已存在，需核对发布状态"));
    } else {
        ModelChangeHead previous;
        if (!readHead(backup,previous,error) || !equal(previous,decode(record["expected"].toObject())))
            return fail(error,QStringLiteral("模型恢复缺少匹配的原始备份，保留发布记录"));
    }
    if (!verifyOwner(record,error)) return false;
    ModelChangeHead preparedHead;
    if (!readHead(prepared,preparedHead,error) || !equal(preparedHead,target))
        return fail(error,QStringLiteral("模型暂存输入已变化，不能恢复发布"));
    if (QFileInfo::exists(active) && !QDir().rename(active,backup))
        return fail(error,QStringLiteral("无法替换正在使用的模型，待恢复记录及原模型已保留"));
    if (!QDir().rename(prepared,active)) {
        if (!QFileInfo::exists(active) && QFileInfo::exists(backup)) QDir().rename(backup,active);
        return fail(error,QStringLiteral("模型发布未完成，保留暂存结果以便重试"));
    }
    return true;
}
bool cleanup(const QString &project,const QJsonObject &record,QString *error) {
    if (!validate(project,record,error) || !verifyOwner(record,error)) return false;
    const QString backup=record["backup"].toString();
    // Legacy readers have no durable reader-lease registry. Retain the old model
    // until a later explicit GC can prove it has no readers/history/recovery roots.
    if (QFileInfo::exists(backup) && !save(backup+".retained.json",record,error)) return false;
    if (!QFile::remove(intentPath(project)))
        return fail(error,QStringLiteral("模型和投影已提交，发布凭据清理待恢复"));
    const QString staging=record["staging"].toString();
    if (!QFileInfo(staging).isSymLink()) {
        QFile::remove(staging+"/.paleo-area-publication.json");
        QDir().rmdir(staging+"/artifacts/metadata");
        QDir().rmdir(staging+"/artifacts"); QDir().rmdir(staging);
    }
    return true;
}
}
bool hasPendingModelPublication(const QString &project) { return QFileInfo::exists(intentPath(project)); }
bool publishPreparedModel(const QString &project,const QString &staging,
                          const ModelChangeHead &expected,QString *error) {
    if (error) error->clear();
    if (hasPendingModelPublication(project))
        return fail(error,QStringLiteral("已有模型发布等待恢复，请先完成恢复"));
    const auto operation=QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::string detail;
    auto lease=paleo::data::tryAcquireModelWriteLease(directory(project).toUtf8().toStdString(),0,detail);
    if (!lease) return fail(error,QStringLiteral("模型正在写入，稍后重试：")+text(detail));
    auto preparedLease=paleo::data::tryAcquireModelWriteLease(directory(staging).toUtf8().toStdString(),0,detail);
    if(!preparedLease)return fail(error,QStringLiteral("模型暂存正在写入，稍后重试：")+text(detail));
    ModelChangeHead current,target;
    if (!readHead(directory(project),current,error) || !equal(current,expected))
        return fail(error,QStringLiteral("ModelHeadConflict：准备期间模型已被修改，未覆盖当前数据"));
    if (!readHead(directory(staging),target,error) ||
        !preservesManagedSources(directory(project),directory(staging),error)) return false;
    if(target.storeId!=expected.storeId)
        return fail(error,QStringLiteral("ModelStoreConflict：暂存模型不属于当前工区模型"));
    if (target.generation==expected.generation)
        return fail(error,QStringLiteral("新模型必须具有独立发布代次"));
    QJsonObject record{{"schema",1},{"operation",operation},{"project",absolute(project)},
        {"staging",absolute(staging)},{"backup",directory(project)+".previous-"+operation},
        {"expected",encode(expected)},{"target",encode(target)}};
    if (!validate(project,record,error) ||
        !save(absolute(staging)+"/.paleo-area-publication.json",record,error) ||
        !save(intentPath(project),record,error)) return false;
    return advance(project,record,error);
}
bool recoverModelPublication(const QString &project,
    const std::function<bool(QString *)> &projectChanges,QString *error) {
    if (error) error->clear();
    if (!hasPendingModelPublication(project)) return true;
    QJsonObject record;
    if (!load(intentPath(project),record,error) || !validate(project,record,error)) return false;
    std::string detail;
    auto lease=paleo::data::tryAcquireModelWriteLease(directory(project).toUtf8().toStdString(),0,detail);
    if (!lease) return fail(error,QStringLiteral("模型发布恢复正在等待写入结束：")+text(detail));
    {
        auto preparedLease=paleo::data::tryAcquireModelWriteLease(directory(record["staging"].toString()).toUtf8().toStdString(),0,detail);
        if(!preparedLease)return fail(error,QStringLiteral("模型暂存恢复正在等待写入结束：")+text(detail));
        if (!advance(project,record,error)) return false;
    }
    if (!projectChanges || !projectChanges(error))
        return fail(error,QStringLiteral("模型已提交，目录投影等待恢复：")+(error?*error:QString()));
    return cleanup(project,record,error);
}
bool recoverModelPublication(DataCatalog &catalog,QString *error) {
    if (!hasPendingModelPublication(catalog.projectDir())) return true;
    if (catalog.refusesWrites()) return fail(error,QStringLiteral("只读工程存在待恢复的模型发布"));
    return recoverModelPublication(catalog.projectDir(),[&](QString *detail) {
        return registerAreaWells(catalog,detail) && registerAreaSources(catalog,detail);
    },error);
}
}
