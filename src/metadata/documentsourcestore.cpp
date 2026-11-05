// 层：数据
#include "documentsourcestore.h"
#include "metastore.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace paleo::document {
namespace {
bool failure(QString *error,const QString &message) { if(error) *error=message;return false; }
bool sql(QSqlDatabase &db,const QString &statement,const QVariantList &args,QString *error) {
    QSqlQuery query(db);query.prepare(statement);for(const auto &arg:args) query.addBindValue(arg);
    return query.exec() || failure(error,query.lastError().text());
}
bool schema(QSqlDatabase &db,const QString &documentId,QString *error) {
    if(db.tables().contains("document_source_meta")) {
        QSqlQuery existing(db);
        if(!existing.exec("SELECT format,document_id FROM document_source_meta WHERE id=1") || !existing.next())
            return failure(error,QStringLiteral("来源文档元数据缺失"));
        if(existing.value(0).toInt()!=1 || existing.value(1).toString()!=documentId)
            return failure(error,QStringLiteral("来源文档格式或身份不兼容"));
    }
    const QStringList statements{
        "CREATE TABLE IF NOT EXISTS document_source_meta(id INTEGER PRIMARY KEY CHECK(id=1),format INTEGER NOT NULL,document_id TEXT NOT NULL,revision INTEGER NOT NULL)",
        "CREATE TABLE IF NOT EXISTS document_source_operations(id TEXT PRIMARY KEY,payload TEXT NOT NULL)"};
    for(const auto &statement:statements) if(!sql(db,statement,{},error)) return false;
    return sql(db,"INSERT OR IGNORE INTO document_source_meta VALUES(1,1,?,0)",{documentId},error);
}
bool metadata(QSqlDatabase &db,const QString &documentId,quint64 *revision,QString *error) {
    QSqlQuery query(db);
    if(!query.exec("SELECT format,document_id,revision FROM document_source_meta WHERE id=1") || !query.next())
        return failure(error,query.lastError().isValid()?query.lastError().text():QStringLiteral("来源文档元数据缺失"));
    if(query.value(0).toInt()!=1) return failure(error,QStringLiteral("不支持的来源文档格式"));
    if(!documentId.isEmpty() && query.value(1).toString()!=documentId) return failure(error,QStringLiteral("来源文档身份与工程不一致"));
    *revision=query.value(2).toULongLong();return true;
}
QSqlDatabase writable(const QString &path,const QString &connection,const QString &documentId,QString *error) {
    return MetaStore::openConnection(path,connection,error,false,MetaStore::kUserVersion,
        [documentId](QSqlDatabase &db,int,int,QString *why){return schema(db,documentId,why);});
}
bool begin(QSqlDatabase &db,QString *error) {return sql(db,"BEGIN IMMEDIATE",{},error);}
bool complete(QSqlDatabase &db,bool ok,QString *error) {
    if(ok && db.commit()) return true;
    if(ok && error) *error=db.lastError().text();db.rollback();return false;
}
}
DocumentSourceStore::DocumentSourceStore(const QString &path)
    :m_path(path),m_connection("paleo_document_sources_"+QUuid::createUuid().toString(QUuid::WithoutBraces)),m_owner(QThread::currentThread()) {}
DocumentSourceStore::~DocumentSourceStore() {
    for(const auto &name:{m_connection,m_connection+"_ro"}) {
        if(!QSqlDatabase::contains(name)) continue;
        {auto db=QSqlDatabase::database(name,false);if(db.isValid()) db.close();}
        if(QSqlDatabase::contains(name)) QSqlDatabase::removeDatabase(name);
    }
}
bool DocumentSourceStore::owner(QString *error) const {
    return QThread::currentThread()==m_owner || failure(error,QStringLiteral("来源存储只能由所属线程访问"));
}
bool DocumentSourceStore::load(const QString &documentId,SourceStoreSnapshot &snapshot,QString *error) {
    snapshot={};snapshot.documentId=documentId;
    if(error) error->clear();if(!owner(error)) return false;
    if(!QFileInfo::exists(m_path)) return true;
    auto db=MetaStore::openConnection(m_path,m_connection+"_ro",error,true);if(!db.isValid()) return false;
    if(!db.tables().contains("document_source_meta")) return true;
    if(!sql(db,"BEGIN",{},error)) return false;
    quint64 revision=0;bool ok=metadata(db,documentId,&revision,error);
    QSqlQuery identity(db);
    if(ok) ok=identity.exec("SELECT document_id FROM document_source_meta WHERE id=1") && identity.next();
    if(ok) snapshot.documentId=identity.value(0).toString();identity.finish();
    QSqlQuery rows(db);int count=0;
    if(ok) ok=rows.exec("SELECT id,payload FROM document_source_operations");
    while(ok && rows.next()) {
        const auto payload=rows.value(1).toByteArray();QJsonParseError parse;
        const auto json=QJsonDocument::fromJson(payload,&parse);
        if(++count>256 || payload.size()>65536 || parse.error!=QJsonParseError::NoError || !json.isObject())
            ok=failure(error,QStringLiteral("来源恢复记录损坏或超过预算"));
        else snapshot.pendingOperations.insert(rows.value(0).toString(),json.object());
    }
    rows.finish();return complete(db,ok,error);
}
bool DocumentSourceStore::ensureIdentity(const QString &documentId,QString *error) {
    if(error)error->clear();if(!owner(error))return false;
    auto db=writable(m_path,m_connection,documentId,error);if(!db.isValid())return false;
    quint64 revision=0;return metadata(db,documentId,&revision,error);
}
bool DocumentSourceStore::writeOperation(const QString &documentId,const QString &id,const QJsonObject &operation,QString *error) {
    if(error) error->clear();if(!owner(error)) return false;
    const auto payload=QJsonDocument(operation).toJson(QJsonDocument::Compact);
    if(id.isEmpty() || payload.size()>65536) return failure(error,QStringLiteral("来源操作记录无效"));
    auto db=writable(m_path,m_connection,documentId,error);if(!db.isValid() || !begin(db,error)) return false;
    quint64 revision=0;bool ok=metadata(db,documentId,&revision,error);
    QSqlQuery existing(db);
    if(ok) {
        existing.prepare("SELECT payload FROM document_source_operations WHERE id=?");existing.addBindValue(id);
        ok=existing.exec();
        if(ok && existing.next()) {
            ok=existing.value(0).toByteArray()==payload;
            if(!ok)failure(error,QStringLiteral("来源操作标识已被不同输入使用"));
            existing.finish();return complete(db,ok,error);
        }
        existing.finish();
    }
    if(ok) {
        QSqlQuery count(db);ok=count.exec("SELECT COUNT(*) FROM document_source_operations") && count.next();
        if(ok && count.value(0).toInt()>=256)ok=failure(error,QStringLiteral("请先恢复未完成的来源操作"));
        count.finish();
    }
    if(ok) ok=sql(db,"INSERT INTO document_source_operations VALUES(?,?)",{id,QString::fromUtf8(payload)},error);
    return complete(db,ok,error);
}
bool DocumentSourceStore::finishOperation(const QString &documentId,const QString &id,QString *error) {
    if(error) error->clear();if(!owner(error)) return false;
    auto db=writable(m_path,m_connection,documentId,error);if(!db.isValid() || !begin(db,error)) return false;
    quint64 revision=0;bool ok=metadata(db,documentId,&revision,error);
    if(ok) ok=sql(db,"DELETE FROM document_source_operations WHERE id=?",{id},error);
    return complete(db,ok,error);
}
}
