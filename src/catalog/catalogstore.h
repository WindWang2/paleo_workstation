// 层：数据
#pragma once

#include "datacatalog.h"

#include <QJsonObject>
#include <QString>

// catalog/catalogstore — catalog.sqlite 持久层（goal/catalog-sqlite）。
//
// 内存里的四表仍是 DataCatalog 的查询事实源。本类只负责落盘：
// 独立文件 <projectDir>/artifacts/metadata/catalog.sqlite（不并入
// project.sqlite），WAL + synchronous=FULL，变更集单事务提交。
// catalog.json 只作为迁移输入；成功迁入后改名为 catalog.json.migrated。
//
// 备份在 open 时做，不在每次 save 时做：增量写若每次整文件拷贝，
// 写路径收益就没了。integrity 通过之后才把当前好库拷进
// catalog.sqlite.bak / .bak.N（保留代数与 JSON 时代相同）。
// 已损坏的主文件不进 .bak 链（#79）。user_version / schema_epoch
// 高于本构建时拒开，不回退旧代（那是降级，不是恢复）。
class CatalogStore
{
  public:
    static constexpr int kSchemaEpoch = 1;
    static constexpr int kJsonSchemaVersion = 1;

    struct Meta
    {
      int revision = 0;
      quint64 mutationSeq = 0;
      int assetSeq = 0;
      int versionSeq = 0;
      int backupKeep = 3;
      bool hasMutationSeq = false;
      bool hasBackupKeep = false;
    };

    struct Tables
    {
      QVector<CatalogEntity> entities;
      QVector<CatalogAsset> assets;
      QVector<CatalogVersion> versions;
      QVector<EntityAssetLink> links;
      Meta meta;
    };

    CatalogStore() = default;
    ~CatalogStore();
    CatalogStore(const CatalogStore &) = delete;
    CatalogStore &operator=(const CatalogStore &) = delete;

    // readOnly：库不存在则成功且 out 为空，不建文件（#80）。
    // 可写且两边都不存在：建空库，revision 记 1。
    // 只有 catalog.json：解析后单事务导入，并把 json 改名为 .migrated。
    // sqlite 已在且带 catalog 表：它是权威，json 原样留着。
    // sqlite 文件在但无表（零长/异构库——崩溃窗口残留）：不算权威，按
    // JSON 迁移 / .bak 恢复链处理，否则工程会以空目录成功打开而真实数据
    // 退化成盘上孤儿文件（#H2）。
    bool openProject(const QString &projectDir, bool readOnly, Tables *out, QString *error);

    bool recovered() const { return m_recovered; }
    QString recoveryReason() const { return m_recoveryReason; }
    bool migratedFromJson() const { return m_migratedFromJson; }
    bool isWritable() const { return m_writable && m_open; }
    // 备份已装入内存，但连接不在主文件上（损坏的 sqlite 还在原路径）。
    // JSON 迁入新库后 recovered() 仍可能为 true，此时本函数为 false——
    // 调用方不得再把新库隔离成 .corrupt。
    bool needsPrimaryRewrite() const { return m_recovered && !m_open; }
    QString sqlitePath() const { return m_sqlitePath; }

    bool begin(QString *error);
    bool upsertEntity(const CatalogEntity &v, QString *error);
    bool upsertAsset(const CatalogAsset &v, QString *error);
    bool upsertVersion(const CatalogVersion &v, QString *error);
    bool upsertLink(int ord, const EntityAssetLink &v, QString *error);
    // 增量删除面（DataCatalog::removeAsset）。同事务内调用次序约定：先
    // upsert 后 delete（净效果 = 已删）。
    bool deleteAsset(const QString &id, QString *error);
    bool deleteVersion(const QString &id, QString *error);
    // 链接行按 ord 主键存储——删除中间行会让后续 ord 整体位移，增量
    // upsert 表达不了，只能整表重写（链接表是行数级，成本可接受）。
    bool replaceAllLinks(const QVector<EntityAssetLink> &links, QString *error);
    bool writeMeta(const Meta &meta, QString *error);
    bool commit(QString *error);
    void rollback();
    bool inTransaction() const { return m_inTxn; }

    // 先把整表写到旁路，replaceAll 提交并 fsync 之后才替换主库。
    // 失败只删旁路：主库与 .bak 不动。损坏文件的隔离是替换前的复制，
    // 不提前删除主库。
    bool rewritePrimary(const Tables &tables, bool quarantineCorrupt, QString *error);

    void close();
    bool resumeConnection(bool readOnly, int expectedRevision, QString *error);

    static QString sqlitePathFor(const QString &projectDir);
    static QString jsonPathFor(const QString &projectDir);
    static QJsonObject toJson(const Tables &tables);
    // schema 不符 → false，*error 含 "unsupported catalog schema"。
    // 坏段版本跳过（qWarning），不因此失败。
    static bool fromJson(const QJsonObject &root, Tables *out, QString *error);
    // PW-077：结构化错误码——schemaUnsupported 置位表示「未来 schema」而非损坏，
    // 调用方据此选恢复策略，不再匹配错误文本。
    static bool fromJson(const QJsonObject &root, Tables *out, QString *error,
                         bool *schemaUnsupported);

  private:
    bool attachWritable(QString *error);
    bool replaceAll(const Tables &tables, QString *error);
    bool createEmpty(Tables *out, QString *error);
    bool migrateFromJson(Tables *out, QString *error);
    bool openExisting(bool readOnly, Tables *out, QString *error);
    bool recover(const QString &primaryError, Tables *out, QString *error);
    // PW-077：schemaRejected 置位 = 主库 user_version 新于本构建（不得走 .bak 恢复）。
    bool connectPrimary(bool readOnly, QString *error, bool *schemaRejected = nullptr);

    bool m_recovered = false;
    QString m_recoveryReason;
    bool m_migratedFromJson = false;
    bool m_writable = false;
    bool m_open = false;
    bool m_inTxn = false;
    QString m_sqlitePath;
    QString m_connectionName;
    QString m_projectDir;
};
