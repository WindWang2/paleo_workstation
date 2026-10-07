// 层：数据
#pragma once

#include <QHash>
#include <QJsonDocument>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <memory>

// 方向 59 头文件卫生：本门面头曾被 27 个 ui TU 直接消费，io/domain 重头
// （lasparser.h / sectiontrace.h / wellrecords.h / wellcompositemodel.h）
// 一律不再出闸。io/lasdoc.h 是唯一保留的 include——它是白名单纯类型
// 门面（Qt 类型 + wellnumeric 常量，零解析入口），且 lasReady 信号的
// QList<LasCurve> 参数在 moc 的 qt_incomplete_metaTypeArray 里必须完整
//（QDebug operator<< 对 QList<T> 做 __is_base_of<QList<T>,T>，不完全 T
// 直接 C2139——方向 59 实测）。SectionDoc 值形态需要 domain/sectiontrace.h，
// 拆细头 sectiondoc.h；其余契约类型只留前向声明（出参全是指针/引用）。
#include "../io/lasdoc.h" // LasCurve/LasDoc/LasHeaderInfo（白名单纯类型门面）

class DataCatalog;
class DataImportService;
class PaleoTask;
class PaleoTaskService;
class SegyReader; // io/segyreader.h——实现侧类型，头文件只留指针容器
struct CatalogEntity;
struct CatalogVersion;
struct TimeDepthTable;    // domain/wellrecords.h（timeDepthAt 出参）
struct WellHeadRecord;    // domain/wellrecords.h（wellHeadsAt 出参）
struct WellTopRecord;     // domain/wellrecords.h（wellTopsAt 出参）

namespace WellComposite
{
struct ComprehensiveWellData; // domain/wellcompositemodel.h（XML 出参）
}

namespace seismic {
class SeismicTaskService;
}

// services/ — 数据页预览的数据门面（W1 预览数据下沉）。
//
// 数据页预览标签（ui/datapreview）只做渲染、收输入、发意图；「解析文件、
// 算标定、解码测线、验 SHA、转 PDF」全部经本门面问答——视图不直接碰
// io/* 解析入口，也不写工程/文件（catalog 读走 catalog()，写操作只剩
// markDownstreamStale 这一处如实标过时）。
//
// 剖面异步语义与旧 UI 内实现逐字节一致：
//   · SegyReader 按 assetId 缓存——换测线只重解码该线，不重建索引；
//   · 每资产世代号——旧请求的结果在发射前就丢弃（UI 无需再判陈旧）；
//   · 新请求协作取消同资产仍在跑的任务；
//   · 外链入库 SHA-256 复验每资产每会话一次；失配 → 失败信号 + 下游
//     DERIVED 如实标过时（catalog 写、幂等，GUI 线程直调）；
//   · 无任务服务时同步旧路径（测试/小环境行为不变）。
class PreviewDocService : public QObject
{
  Q_OBJECT
  public:
    explicit PreviewDocService(DataImportService *svc, QObject *parent = nullptr);
    // out-of-line 析构：m_seismicTaskSvc 是 incomplete-type unique_ptr。
    ~PreviewDocService() override;

    DataImportService *importService() const { return m_svc; }
    DataCatalog *catalog() const;
    QString absolutePathForVersion(const CatalogVersion &version) const;
    CatalogVersion versionForPreview(const QString &versionId) const;
    QString entityIdForAsset(const QString &assetId) const;
    // ---- 壳只读出口（W2：主窗不再 include io/dataimportservice.h）----
    QString catalogOpenError() const;
    QStringList assetIds(const QString &type = QString()) const; // svc->assets
    QString assetSource(const QString &assetId) const;           // svc->assetSource
    // B2（wave/deepen-perf）：单文件导入门面——导入队列 runner 的生产绑定面
    //（视图层不直接 include io/dataimportservice.h）。语义直通
    // DataImportService::importFile：返回资产 id（空 = 失败 + *error）。
    QString importSingleFile(const QString &kind, const QString &sourcePath,
                             QString *error = nullptr);
    // 审计 02 M-8：单文件导入的 produce/commit 拆分面（导入队列 runner 用）。
    // prepare 在 GUI 线程调（拷 catalog staging 副本）；produce() 可在任意线程
    // 跑（只碰 staging，不碰活 catalog）；commit() 回 GUI 线程一处入库，返回
    // 资产 id（空 = 失败 + *error；提交期 catalog 已被别处改动 → 失败并提示
    // 重试，交队列 D8.2 自动重试）。服务未就绪 → nullptr。
    struct SingleFileImportJob
    {
      std::function<void()> produce;
      std::function<QString(QString *error)> commit;
    };
    std::shared_ptr<SingleFileImportJob> prepareSingleFileImport(const QString &kind,
                                                                 const QString &sourcePath);
    // 外链「重新定位文件」执行半边（datalist/标签共用口径）。
    QString relocateVersionSource(const QString &versionId, const QString &pickedPath,
                                  QString *error);

    // ---- 任务服务（D1 异步解码；空 = 同步旧路径）----
    void setTaskService(PaleoTaskService *svc);
    PaleoTaskService *taskService() const { return m_taskSvc; }
    seismic::SeismicTaskService *seismicTaskService() const { return m_seismicTaskSvc.get(); }

    // ---- 工程参数门面（domain/arearules.h 的视图侧出口）----
    static QString targetHorizon();
    static int onnxGridRows();
    static int onnxGridCols();

    // ---- 文件解析门面（同步读；打不开文件 → false + errorString 原文）----
    // lasAt 是纯解析——静态出口（连井剖面等无门面实例的视图也走它）。
    static bool lasAt(const QString &absPath, QStringList *names,
                      QList<LasCurve> *curves, QString *error = nullptr);
    // header-only 快速解析（T1 LAS 解阻）：只读 ~V/~W/~C 到 ~A 段头为止——
    // 代价与头部行数成正比、与数据行数无关，GUI 线程取曲线名不再整文件
    // 解析。曲线名与 lasAt 产出的 names 逐项一致；~A 缺失不算失败
    // （sawAscii 如实报）。WRAP YES / 无 ~C 与 lasAt 同一拒绝语义。
    static bool lasHeaderAt(const QString &absPath, LasHeaderInfo *out,
                            QString *error = nullptr);
    static bool wellHeadsAt(const QString &absPath, QVector<WellHeadRecord> *out,
                            QString *error = nullptr);
    static bool wellTopsAt(const QString &absPath, QVector<WellTopRecord> *out,
                           QString *error = nullptr);
    static bool timeDepthAt(const QString &absPath, TimeDepthTable *out,
                            QString *error = nullptr);

    // ---- GeoJSON 门面（io/geojsonaffine 的视图侧出口）----
    static bool geoJsonBounds(const QString &absPath, double bounds[4],
                              QString *error = nullptr);
    // 读整份 GeoJSON（要素表预览的消费形态：属性键集合由视图自己归并）。
    static bool geoJsonDocumentAt(const QString &absPath, QJsonDocument *out,
                                  QString *error = nullptr);
    // F3（goal/perf-systematize 簇2）：属性面板统计出口——bounds + 要素计数
    // + 属性键集合，两遍流式增量扫描（io/streaming：无 DOM、不整读、内存
    // 平坦）。属性键为排序去重集合（确定性）。bounds 缺坐标只置
    // hasBounds=false 不算失败（与旧视图口径一致）。
    struct GeoJsonSummary
    {
        bool hasBounds = false;
        double bounds[4] = {0, 0, 0, 0}; // [minX, minY, maxX, maxY]
        qint64 featureCount = 0;
        QStringList propKeys; // 排序去重
    };
    static bool geoJsonSummaryAt(const QString &absPath, GeoJsonSummary *out,
                                 QString *error = nullptr);
    // 仿射预览：src 四角经 params（tx/ty/sx/sy/rotDeg）变换后的包围盒。
    static void affinePreviewBounds(const double src[4], const QVariantMap &params,
                                    double lo[2], double hi[2]);

    // ---- 单井综合柱状图 XML 门面（io/wellcompositexml 的视图侧出口）----
    static bool wellCompositeAt(const QString &absPath,
                                WellComposite::ComprehensiveWellData *out,
                                QString *error = nullptr);

    // ---- 外链 SHA-256 闸门（§3/B 包）----
    // 非托管版本带 sha256 时先验后发；会话内每资产只验一次（resetSha 供
    // 「重新定位文件」后重验）。失配时由服务内部把下游 DERIVED 如实标过时。
    bool verifyExternalSha(const QString &assetId, const CatalogVersion &version,
                           QString *error) const;
    void resetSha(const QString &assetId);
    bool markDownstreamStale(const QString &versionId, const QString &reason,
                             QString *error);

    // ---- 文档 PDF 预览（原件 pdf 直接用；office 格式经 soffice 懒转换）----
    enum class DocPdfState { None, Pending, Ready, Failed };
    void ensureDocumentPdf(const QString &assetId);
    DocPdfState documentPdfState(const QString &assetId) const;
    QString documentPdfPath(const QString &assetId) const;
    QString documentPdfError(const QString &assetId) const;

    // ---- 剖面标定快照（D61 标定井：分层点坐标→TD 表插值→初始测线）----
    // 一次算好 UI 需要的全部派生量；计算口径与旧 UI 内实现一致：
    // catalog 序第一口有目标层位分层的井；TD 表取该井已决主 time_depth；
    // 分层点 TVD 空退 MD；失败原因如实写 statusText，绝不造时间。
    struct TieMarker
    {
      bool haveTop = false;
      QString wellId;
      QString wellName;
      QString horizon;       // AreaRules::active().targetHorizon
      bool ok = false;       // 插值成功 → timeMs 有效
      double timeMs = 0.0;
      QString statusText;    // ok 为空；否则「无时深表/超出时深表/时深表无序」
      bool hasCoords = false;
      double x = 0.0;
      double y = 0.0;
      int initialInline = -1; // 标定井所在 inline（判不出 = -1，控件回 min）
    };
    TieMarker seismicTieMarker(const QString &assetId) const;

    // ---- 测线解码（见类注释的异步语义）----
    // SectionDoc 完整定义在 sectiondoc.h（方向 59 拆细头）——值成员需要
    // domain/sectiontrace.h，门面头不再出闸重头；信号/引用形参用本嵌套
    // 前向声明即可。要触碰成员的消费 TU include services/sectiondoc.h。
    struct SectionDoc;
    // 请求解码一条测线；同资产重复请求自动作废上一代。结果经信号回来
    // （无任务服务时同步执行、请求返回前信号已发——与旧同步路径一致）。
    void requestSection(const QString &assetId, const QString &versionId,
                        const QString &absPath, bool managed, const QString &sha256,
                        bool isInline, int lineNo);
    // 标签关闭时释放该资产的索引缓存/世代号/进行中任务。
    void releaseSection(const QString &assetId);
    // 后台从完整道索引恢复测区四角，旧工程中的缩小角点不再决定全景范围。
    void requestSurveyBounds(const QString &assetId,
        std::function<void(const QVector<QPair<double, double>> &, const QString &)> done);

    // ---- LAS 数据异步填充（T1：GUI 线程零同步整文件解析）----
    // requestLas 在任务池整份解析 LAS（协作取消 + 每 key 世代号防陈旧），
    // 结果经 lasReady 回 GUI。key 是调用方稳定的键（资产 id / 井 id）——
    // 同 key 新请求自动作废旧代，旧任务请求取消。无任务服务时同步执行、
    // 返回前信号已发（与 requestSection 同一降级口径）。GUI 消费路径建议
    // 先用 lasHeaderAt 铺曲线名，再 requestLas 补数据行。
    // siblingPaths 与 absPath 在同一次后台任务里解析数据体。某个兄弟文件
    // 解析失败则跳过该文件，不让本次请求失败。空列表 = 只解析 absPath。
    void requestLas(const QString &key, const QString &absPath,
                    const QStringList &siblingPaths = {});
    // 最近一次成功 requestLas 的兄弟文件（失败的不在表里）。当前文件仍由
    // lasReady 的 curves 交付。只在 lasReady 槽里读；releaseLas / 同 key
    // 新请求会清掉。
    QHash<QString, LasDoc> lasSiblingDocs(const QString &key) const;
    // 释放该 key 的世代号；进行中的解析请求取消——结果没人等了。
    void releaseLas(const QString &key);
    // #154：工程切换前清空所有按 assetId 键控的会话缓存（SEG-Y 索引读者、
    // SHA 已验集、金字塔状态、LAS 兄弟文档）并取消在途解码/解析/预热任务。
    // assetId（ast-N）在不同工程间会重复——不清会把旧工程的读者/已验结论
    // 套到新工程同名资产上。世代号不清零而是逐键 +1：迟到的旧代结果与新
    // 工程的第一代请求不会撞号。
    void resetProjectState();

    // ---- B3（wave/deepen-perf）：栅格金字塔版本预热（quiet 异步）----
    // 对资产当前版本的栅格（受管 tif/png/jpg；horizon DERIVED tif 同口径）
    // 后台构建瓦片金字塔（Lazy→Eager 补齐）+ GDAL 外部 .ovr 概览——
    // 完成后 QGIS 渲染自动走概览，大图全图首渲不再整幅降采样重读。
    // 幂等：已就绪/在途/已失败（会话内）不重复；同一资产在途时新请求合并。
    // 无任务服务时同步执行（测试/小环境）。外链源不建 .ovr（不写用户目录
    // 外的边车），仅工程内瓦片缓存。
    void ensureRasterPyramidVersion(const QString &assetId);
    // 会话内是否已就绪（含同步路径）；视图可据此决定是否提示。
    bool rasterPyramidReady(const QString &assetId) const;

    // ---- D1.3 批量预取：后台低优先级把一批 LAS 解析进缓存 ----
    // 不发 lasReady（调用方按需再 requestLas 即命中缓存）；任务服务空时同步
    // 逐个装载（测试/小环境）。返回提交的任务数（0 = 无任务服务同步完成）。
    int prefetch(const QStringList &absPaths);

  signals:
    // 解码成功（已判陈旧——到达的必然是最新一代）。
    void seismicSectionReady(const QString &assetId,
                             const PreviewDocService::SectionDoc &doc);
    // 解码失败（原因如实；SHA 失配的下游标过时已在服务内做完）。
    void seismicSectionFailed(const QString &assetId, const QString &reason);
    void seismicSectionCancelled(const QString &assetId);
    // DataImportService 文档 PDF 信号的转发（UI 重建对应标签）。
    void documentPdfReady(const QString &assetId);
    void documentPdfFailed(const QString &assetId, const QString &error);
    // DataImportService 信号的壳侧转发（主窗只认门面）。
    void catalogOpenFailed(const QString &error);
    // catalog open() 经 .bak 回退恢复（T5）——「已从备份恢复，主文件损坏」
    // 的用户可见告警面；catalog 本身可用。
    void catalogRecoveredFromBackup(const QString &reason);
    void assetImported(const QString &kind, const QString &assetId,
                       const QString &layerId);
    // LAS 数据异步填充结果（requestLas；已判陈旧——到达的必然是最新一代）。
    void lasReady(const QString &key, const QStringList &names,
                  const QList<LasCurve> &curves);
    void lasFailed(const QString &key, const QString &reason);
    void lasCancelled(const QString &key);
    // B3：栅格金字塔版本预热终态（ok = 瓦片 + 概览均就绪或无需建）。
    void rasterPyramidFinished(const QString &assetId, bool ok);

  private:
    DataImportService *m_svc = nullptr;
    PaleoTaskService *m_taskSvc = nullptr;
    std::unique_ptr<seismic::SeismicTaskService> m_seismicTaskSvc;

    // D1 异步解码：按资产的索引缓存（每资产只 open/SHA 一次）、世代号
    //（陈旧结果发射前丢弃）、进行中任务指针（新解码请求取消旧任务）。
    QHash<QString, std::shared_ptr<SegyReader>> m_segyReaders;
    QHash<QString, int> m_decodeSeq;
    QHash<QString, QPointer<PaleoTask>> m_decodeTask;
    int m_surveyGeneration = 0;
    QPointer<PaleoTask> m_surveyTask;
    // assetId → 本会话已过 SHA 复验（mutable：verifyExternalSha 是 const——
    // 会话级缓存不算对象逻辑状态，seismicTieMarker 等 const 读路径可用）。
    mutable QHash<QString, bool> m_shaVerified;

    // T1 LAS 异步填充：按 key 的世代号（陈旧结果发射前丢弃）与进行中
    // 任务指针（新解码请求取消旧任务）——与 m_decodeSeq/m_decodeTask 同一模式。
    QHash<QString, int> m_lasSeq;
    QHash<QString, QPointer<PaleoTask>> m_lasTask;
    QHash<QString, QHash<QString, LasDoc>> m_lasSiblings;

    // B3 栅格金字塔预热：按资产的会话状态（Ready/Failed 不重复）与在途
    // 任务指针（同资产新请求合并——在途即视为已受理）。
    enum class PyramidState { None, InFlight, Ready, Failed };
    QHash<QString, int> m_pyramidState;
    QHash<QString, QPointer<PaleoTask>> m_pyramidTask;
};
// SectionDoc 的 Q_DECLARE_METATYPE 随完整定义移至 sectiondoc.h（方向 59）。
