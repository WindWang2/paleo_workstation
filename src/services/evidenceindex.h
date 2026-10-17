// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <functional>

class DataCatalog;
struct CatalogVersion;

// evidenceindex/ — G08/G12 证据反向索引（可重建检索结构，非第二事实源）。
// 图斑级人工证据只存在成果 GPKG 的要素属性里（facies_evidence /
// paleo_interpretation / paleo_feature_uid / paleo_identity_review，记录结构
// 见 domain/interpretationevidence.cpp）。本索引把「证据 id → 哪些图斑引用
// 它」倒排进 <projectDir>/artifacts/evidence-index.sqlite，供检索与复核定位
// 使用；任何时刻删除该文件并 synchronize 即可整库重建——目录与 GPKG 仍是
// 唯一事实源，这里不产生新事实，unlinked 历史记录按原样入库（可检索 ≠
// 有效关联）。
//
// 读取用 GDAL/OGR C API（paleo_services 经 paleo_deps 已链 GDAL）；索引本体
// 用 QtSql QSQLITE（与 catalog.sqlite 同口径：WAL + bound value，无手拼 SQL
// 值）。单线程同步调用（调用方保证 GUI 线程或串行）。
namespace paleo::evidenceindex {

// 一行 = 一个图斑上的一条证据记录；解释字段（interpretation_id 等）随该
// 图斑的每条记录重复出现。无记录但有待复核标记/解释的图斑占一条
// 「图斑级」行（evidence_id 为空）。
struct Entry {
  QString versionId, assetId, layerId, title, horizon;   // 成果版本与图层
  QString featureUid;                                     // 图斑稳定 UID
  QString evidenceId, evidenceType, role, text;           // facies_evidence 记录
  QString interpretationId, assertionId, reviewState;     // paleo_interpretation
  QString sourceVersion, sourceFeatureUid;                // 记录钉定的来源
  QString actorId;
  bool identityReview = false;                            // paleo_identity_review
  bool unlinked = false;                                  // 记录 tombstone
};

// 同步读取上限（防病态文件拖死 UI）；命中上限时该版本在 indexed_versions
// 记 truncated=1 并在 stats().truncated_versions 汇总——截断是登记事实，
// 不是静默部分索引。
struct SyncOptions {
  int maxFeaturesPerVersion = 200000;    // 每版本要素读取上界
  qint64 maxRowsPerVersion = 2000000;    // 每版本展开行数上界（特征×记录）
};

class Index {
  public:
    Index() = default;
    ~Index(); // 关闭并注销 QSqlDatabase 连接
    Index(const Index &) = delete;
    Index &operator=(const Index &) = delete;

    // 索引文件 <projectDir>/artifacts/evidence-index.sqlite；目录不存在则建。
    // user_version 过新（未来 schema）→ false + error；过旧（v1）→ 整库
    // 重建为 v2（索引是可重建检索结构，drop 后下一轮 sync 全量重灌）。
    bool open(const QString &projectDir, QString *error = nullptr);
    void close();
    bool isOpen() const { return m_open; }

    // 增量同步：扫描 catalog 中 mapping_product 且 layer_type=vector 的版本
    // （kind 含 facies_polygons / 其他矢量成果），按 (version_id, sha256)
    // 跳过已索引；sha 变化或版本消失 → 整版本条目替换/删除。返回变更版本数
    // （新增+替换+删除）；取消 → -1。解析失败的版本记入 indexed_versions 带
    // error 列，synchronize 不中断；单条损坏 JSON 记录跳过并计数
    // （stats().corrupt_records）。versions 重载供后台 worker 使用——
    // DataCatalog 只在 GUI 线程，快照必须在发起线程取好。
    int synchronize(const DataCatalog *catalog, const QString &projectDir,
                    const std::function<bool()> &cancelled = {},
                    QString *error = nullptr,
                    const SyncOptions &options = {});
    int synchronize(const QVector<CatalogVersion> &versions,
                    const QString &projectDir,
                    const std::function<bool()> &cancelled = {},
                    QString *error = nullptr,
                    const SyncOptions &options = {});

    // ---- 查询（全部有界 + offset/limit，limit cap 1000）----
    // 证据 id → 引用它的图斑条目（含 unlinked 历史行，见 Entry.unlinked）。
    QVector<Entry> featuresForEvidence(const QString &evidenceId,
                                       int limit = 200, int offset = 0) const;
    // 解释 id → 该解释所在图斑的条目（每条记录一行 + 图斑级行）。
    QVector<Entry> featuresForInterpretation(const QString &interpretationId,
                                             int limit = 200, int offset = 0) const;
    // 来源版本 id → 钉定该版本的条目（含已解除关系的历史行）。
    QVector<Entry> featuresReferencingSource(const QString &sourceVersionId,
                                             int limit = 500, int offset = 0) const;
    // horizon 内待复核：reviewState 为 NeedsReview/Proposed 或 identityReview。
    QVector<Entry> pendingReview(const QString &horizon,
                                 int limit = 500, int offset = 0) const;
    // horizon 内被反驳关联：role 含 CONTRADICTED。
    QVector<Entry> contradicted(const QString &horizon,
                                int limit = 500, int offset = 0) const;
    // horizon 内过期/缺失来源：来源类型要求钉定 source_version 却为空，或
    // 记录带 source_missing / source_feature_missing 标记（unlinked 行除外）。
    QVector<Entry> staleSources(const QString &horizon,
                                int limit = 500, int offset = 0) const;
    // horizon 内被有效（unlinked=0）记录钉定过的全部来源版本 id——供调用方
    // 按目录核验「来源版本是否还在」。
    QStringList distinctSourceVersions(const QString &horizon,
                                       int limit = 1000) const;
    // 全文检索：text / actorId / evidenceId 的 LIKE 命中（ASCII 大小写不
    // 敏感）。空串 → 空结果（不是全库匹配）。
    QVector<Entry> search(const QString &text,
                          int limit = 500, int offset = 0) const;

    // 同类计数（分页容量提示）：queryName 与上述查询同名
    // （featuresForEvidence/featuresForInterpretation/featuresReferencingSource/
    // pendingReview/contradicted/staleSources/search），argument 为对应参数
    // （horizon / id / 检索词）。未知名字 → -1 并置 lastError。
    int countFor(const QString &queryName, const QString &argument) const;

    // {versions_indexed, entries, last_sync, errored_versions, corrupt_records,
    //  truncated_versions}
    QVariantMap stats() const;
    QString lastError() const { return m_lastError; }

  private:
    bool connection(QString *error) const; // 确保 QSqlDatabase 可用
    QString m_connection; // QSqlDatabase 连接名（避免持有 QSqlDatabase 值）
    QString m_dir;
    mutable QString m_lastError;
    bool m_open = false;
};

} // namespace paleo::evidenceindex
