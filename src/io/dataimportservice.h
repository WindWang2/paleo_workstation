// 层：数据
#pragma once
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <type_traits>
#include <utility>
#include <QMetaObject>
#include <QThread>
#include "../catalog/datacatalog.h" // catInvoke 模板需完整类型（thread()/invokeMethod）
#include "../domain/importrows.h"   // FolderPreviewRow / FolderRowResult（domain 瞬态 DTO）

struct LayerDeclaration;
class PaleoProjectStore;
class QProcess;
struct PlannedItem; // io/ingestplan.h（前向声明——C 包 plan 项按引用传）
struct IngestPlan;  // io/ingestplan.h（planFor 返回值；定义侧 include）

// io/ — DataImportService 按 project_area 数据契约（docs/PROJECT_AREA_PLAN.md §3）
// 导入外部文件：分类 → 解析元数据 → 解析/创建实体 → 受管 RAW 复制（边复制边算
// SHA-256，落盘只读）或外部链接 → 显式 entity_asset_link →（层位）派生时间栅格。
// catalog.json 是唯一主存储；.dat 不再交给 OGR，不再声明成矢量。
class DataImportService : public QObject
{
  Q_OBJECT
  public:
    struct ImportOptions
    {
      bool linkExternal = false; // 用户明确选择「链接外部」才不复制；SEG-Y 一律外链
      QString forceType;         // 非空 → 跳过分类器结果改用此类型（确认表「改类型」）
    };

    explicit DataImportService(PaleoProjectStore *store = nullptr, QObject *parent = nullptr);
    ~DataImportService() override;

    void setProjectDir(const QString &dir);   // where the project lives

    // T20a：catalog 打开失败面（audit row 36）。setProjectDir 里 open() 失败 →
    // 发 catalogOpenFailed + 记 catalogOpenError()；此后 catalog 处于拒绝写入
    // 态（catalog()->refusesWrites()），每个 mutator/导入都如实失败，不会拿
    // 空 catalog 覆盖坏文件。
    QString catalogOpenError() const { return m_catalogOpenError; }
    bool catalogWritable() const { return m_catalog && m_catalogReady; }

    // 导入结果（§3 dedup）：outcome 区分新建入库与「字节已在库」；
    // linkAttached 记 dedup 时是否补上了之前未决的主关联；message 是 UI 文案
    // （「字节已在库 · 已补上关联」/「字节已在库 · 没有新的关联」）。
    enum class ImportOutcome { Failed, Imported, AlreadyStored };
    struct ImportResult
    {
      QString assetId;                      // Imported=新资产；AlreadyStored=已存在资产
      ImportOutcome outcome = ImportOutcome::Failed;
      bool linkAttached = false;
      QString message;
    };

    // 新契约入口。返回资产 id；失败返回空串并设置 *error。
    // （dedup 命中时返回已存在资产 id——用 importProjectFileEx 区分两种结局。）
    QString importProjectFile(const QString &sourcePath, QString *error = nullptr);
    QString importProjectFile(const QString &sourcePath, const ImportOptions &options,
                              QString *error = nullptr);
    // 带完整结果的入口：dedup（AlreadyStored）不新建资产/版本/主关联。
    ImportResult importProjectFileEx(const QString &sourcePath, QString *error = nullptr);
    ImportResult importProjectFileEx(const QString &sourcePath, const ImportOptions &options,
                                     QString *error = nullptr);

    // ---- 文件夹导入（§3/autoplan：「导入工区文件夹」的后端半边）----
    // 递归走所选目录（分类依赖 井位/井分层/时深/层位 路径段——真工区文件全在
    // 子目录里，时深还在 TD 二级子目录）。只收普通文件：目录只下钻不出行，
    // fifo/socket 等非普通文件和指向所选根目录之外的符号链接标 Skipped；
    // 一行失败不中断其余文件。隐藏文件（.preview_cache 之类）不进表。
    // 两阶段：先处理全部 well_head 行（井建齐），其余文件再对已齐的井集解
    // 析——LAS 排序在井口前也照常挂到 A1。返回行按处理序排：well_head 行在
    // 前、其余行随后（各按路径排序）、Skipped 行缀在最后。
    // 行类型 = domain/importrows.h 的裸类型（二期别名已下线，全仓库直呼）。
    QVector<FolderRowResult> importFolder(const QString &dirPath, QString *error = nullptr);
    // typeOverrides：确认表里用户改过类型的行——key 是源路径，value 是目标类型。
    // 只认分类器词表内的类型（projectClassifierTypes()）；非法值忽略，行按
    // 分类器原类型处理。阶段划分按生效类型算——改成 well_head 的行回阶段 1（D5）。
    // progress（可选）：每处理完一行回调一次 (done, total, 该行路径)——在
    // 执行线程上直调；返回 false = 协作取消（返回已处理的行 + error 记
    // 「已取消」）。异步调用方把它接 PaleoTask::reportBytes + cancelRequested。
    QVector<FolderRowResult> importFolder(
        const QString &dirPath, QString *error,
        const QMap<QString, QString> &typeOverrides,
        const std::function<bool(int done, int total, const QString &path)> &progress =
            {});
    // 同上 + 「仍导入」改判（T2 确认表跳过策略）：forceImportPaths 里的
    // 「重复→跳过」行改判 as_new_version——同字节重登记交内部 dedup
    // （AlreadyStored + 补挂），不破坏 plan 期其余决策。
    QVector<FolderRowResult> importFolder(
        const QString &dirPath, QString *error,
        const QMap<QString, QString> &typeOverrides,
        const QStringList &forceImportPaths,
        const std::function<bool(int done, int total, const QString &path)> &progress =
            {});

    // 单行重导（确认表「重试」）：forceType 口径同 typeOverrides 的单值
    // （空=按分类器；非法值忽略）。返回与 importFolder 相同口径的行结果；
    // 失败时 *error 带行 message。
    FolderRowResult importFolderRow(const QString &sourcePath,
                                    const QString &forceType = QString(),
                                    QString *error = nullptr);

    // 确认表预览：与 importFolder 同一枚举/分类口径，只列行不导入。
    // skipped=true 的行是软链逃逸/非普通文件（预览里灰显、不可改类型）。
    // decision 是 plan 期决策（"skip"=重复→跳过 等），确认表逐行显示。
    // displayType/typeEditable/typeVocab 已在预览期按分类器谓词回填。
    QVector<FolderPreviewRow> previewFolder(const QString &dirPath, QString *error = nullptr);
    // 同上 + 扫描期进度回调（每见一个源文件一次：(已见数, 路径)；返回
    // false = 协作取消）。T2：FolderImportWorkflow 在任务池里调它——扫描/
    // 分类/哈希全在 worker，GUI 只收进度信号。
    QVector<FolderPreviewRow>
    previewFolder(const QString &dirPath, QString *error,
                  const std::function<bool(int filesSeen, const QString &path)> &scanProgress);

    // ---- C 包 IngestPlan 执行面（docs/DATA_FABRIC_ADOPTION.md）----
    // 执行单条 plan 项——executeIngestPlan 逐项调它；确认表「重试」合成的
    // 单项也走这里。decision==skip 或 (path,sha) 已注册 → Skipped 行；
    // as_new_version 有意绕过幂等检查（同字节重登记交内部 dedup：AlreadyStored
    // + 补挂）。返回与 importFolder 相同口径的行结果。
    FolderRowResult executePlannedItem(const PlannedItem &item, QString *error = nullptr);

    // 旧签名（mainwindow importRequested 接线）：kind 仅用于信号，不再决定行为。
    QString importFile(const QString &kind, const QString &sourcePath, QString *error = nullptr);

    // ---- B3（wave/deepen-perf）：栅格金字塔批量接线 ----
    // 工程级瓦片金字塔根 <project>/artifacts/pyramids（RasterPyramidService；
    // 源身份 mtime/size 变化自动重建）。Lazy ensure：只铺目录 + 状态表，
    // 瓦片由消费侧首次取用时生成——导入期近零开销。
    QString pyramidCacheDir() const;
    // 批量 ensure（RasterPyramidService::ensure Lazy）。返回成功条数；单文件
    // 失败不中断（记 warning）；progress(done, total, path) 返回 false = 协作
    // 取消（已 ensure 的保留）。空工程目录 → 0（无缓存根，如实不做事）。
    int ensureRasterPyramids(const QStringList &absPaths, QString *error = nullptr,
                             const std::function<bool(int done, int total,
                                                      const QString &path)> &progress = {});
    // GDAL 外部概览（<file>.ovr 边车）构建——消费侧渲染加速的真实生效面：
    // QGIS/GDAL 渲染自动发现并使用 .ovr，全图首渲不再整幅降采样重读。
    // 层级 2,4,8,… 至最小维 <512px；小图无层级 = 立即成功（无事可做）。
    // 外部 .ovr 绝不改受管 RAW 字节（入库 SHA 留底保持有效）。progress 同上
    //（单文件粒度）+ buildProgress(0..1) 经 ensureRasterPyramidVersion 接任务。
    int buildRasterOverviews(const QStringList &absPaths, QString *error = nullptr,
                             const std::function<bool(int done, int total,
                                                      const QString &path)> &progress = {});
    // 单文件版（含 0..1 构建进度回调，返回 false = 取消）。
    bool buildRasterOverviews(const QString &absPath, QString *error = nullptr,
                              const std::function<bool(double fraction)> &buildProgress = {});

    DataCatalog *catalog() const { return m_catalog; }

    // 版本路径 → 工程内绝对路径（外链原样返回）。
    QString absolutePath(const QString &assetId) const;
    // P4：SEG-Y 道头索引缓存目录（<project>/artifacts/index/segy；空工程 = 空）。
    QString indexCacheDir() const;
    // 指定版本的绝对路径——文档标签取 RAW 原件用（currentVersion 可能已是
    // DERIVED 转换件）。
    QString absolutePathForVersion(const CatalogVersion &version) const;

    // ---- 文档 PDF 预览（§4 升级：soffice headless → DERIVED 版本，懒转换）----
    // document 资产（doc/docx/ppt/pptx）首次预览时调用 ensureDocumentPdf：
    // LibreOffice headless 转出的 PDF 落为受管 DERIVED 版本（parent=RAW）。
    // 幂等——已就绪/在途/已败（会话内缓存）时不动；结果经 documentPdf* 信号。
    enum class DocPdfState { None, Pending, Ready, Failed };
    void ensureDocumentPdf(const QString &assetId);
    DocPdfState documentPdfState(const QString &assetId) const;
    QString documentPdfPath(const QString &assetId) const;   // Ready 时有效
    QString documentPdfError(const QString &assetId) const;  // Failed 时有效
    // 部署/测试注入：替代 PATH 上的 soffice/libreoffice 探测（可指向 stub）。
    // 空串 = 强制不可用（走 Failed 降级）。
    void setDocumentConverterProgram(const QString &program);

    // ---- 兼容面（bottom-dock 面板仍在用）----
    QStringList assets(const QString &type = QString()) const;   // asset ids（按分类类型过滤）
    QString assetSource(const QString &assetId) const;           // 当前版本路径（外链绝对）

    // ---- wave4/runtime-resilience：外链源重定位（TODOS P3 恢复路径）----
    // RAW 外链版本（managed=false）源文件被移动/重命名后的恢复入口。流式重算
    // 新文件 SHA-256，与该版本入库时留底一致才接受：一致 → 追加一条指向新
    // 路径的同内容外链版本记录（版本不可变，不覆盖旧记录；extra.relocatedFrom
    // 留血统），持久化进 catalog.json，currentVersion 从此解析到新路径；
    // 不一致 → 拒绝，error 写明「文件内容与原版本不符」，catalog 不动。
    // 新路径落在工程目录内也仍按 external 记（不升级为 managed）。
    // 返回新版本 id；同一文件已在原位（幂等）回原 id；失败回空串 + *error。
    QString relocateVersionSource(const QString &versionId, const QString &newPath,
                                  QString *error = nullptr);

  signals:
    void layerDeclared(const LayerDeclaration &decl);
    void imported(const QString &kind, const QString &assetId, const QString &layerId);
    void importFailed(const QString &kind, const QString &path, const QString &error);
    // setProjectDir 里 catalog open 失败即发；成功打开后 catalogOpenError() 清空。
    void catalogOpenFailed(const QString &error);
    // catalog open() 经 .bak 回退恢复成功（T5）——主文件损坏原因随行，UI
    // 状态面据此告警。catalog 本身可用（读面正常、可续存）。
    void catalogRecoveredFromBackup(const QString &reason);
    void documentPdfReady(const QString &assetId);
    void documentPdfFailed(const QString &assetId, const QString &error);

  private:
    // T2 plan 期搬出 GUI：catalog 线程上 COW 快照（O(1)）→ 当前线程跑
    // buildIngestPlan（扫描/分类/哈希/身份匹配/去重复核）。worker 调用即
    // plan 期整体离 GUI 线程——不再 BlockingQueuedConnection 把整段扫描
    // marshal 回 GUI。scanProgress 可选（plan 构建期逐文件进度 + 取消）。
    IngestPlan planFor(
        const QString &root,
        const std::function<bool(int filesSeen, const QString &path)> &scanProgress =
            {}) const;

  public:
    // 线程规则（D1b/D1c 异步导入）：import* 系列可在 worker 线程执行——内部
    // 对 catalog 的每一次读写经 catInvoke marshal 回 catalog 所在线程（GUI）。
    // GUI 线程调用 = 直调零成本；worker 线程 = 异步 QueuedConnection (void) 或
    // BlockingQueuedConnection (非 void)。catalog 因此永远只被它自己的线程触碰。
    template <typename Fn> auto catInvoke(Fn &&fn) const
    {
      using R = std::invoke_result_t<Fn>;
      if (QThread::currentThread() == m_catalog->thread())
      {
        if constexpr (std::is_void_v<R>)
        {
          fn();
          return;
        }
        else
          return fn();
      }
      if constexpr (std::is_void_v<R>)
      {
        QMetaObject::invokeMethod(m_catalog, std::forward<Fn>(fn),
                                  Qt::QueuedConnection);
      }
      else
      {
        R result{};
        const bool ok = QMetaObject::invokeMethod(m_catalog, [&result, &fn] { result = fn(); },
                                                  Qt::BlockingQueuedConnection);
        if (!ok)
        {
          qWarning("DataImportService::catInvoke: deadlock or dispatch failure detected");
        }
        return result;
      }
    }

  private:

    struct WellBind
    {
      QString entityId;   // 恰好一个匹配
      bool unresolved = false;
      QStringList candidates; // 0/2+ 候选时的集合
    };
    // 单行导入 → FolderRowResult（importFolder 每行与「重试」共用口径）：
    // effectiveType 与 classifiedType 不同时经 forceType 下传。
    FolderRowResult folderRowFor(const QString &path, const QString &classifiedType,
                                const QString &effectiveType);

    WellBind resolveWell(const QString &name) const;

    // 单文件导入实体（plan 化前的 importProjectFileEx 主体——分类→dedup→
    // 受管 RAW/外链→实体解析→关联，语义原样未动）。由单文件 wrapper 与
    // executePlannedItem 调用。
    ImportResult importOneFile(const QString &sourcePath, const ImportOptions &options,
                               QString *error);

    // shp 族成员补齐：把主件之外的成员拷进指定受管版本目录（幂等——已存在
    // 跳过；外链版本不拷，源目录本是一族）。失败成员名附进 *messageOut。
    void copyBundleMembersIntoVersion(const PlannedItem &item, const QString &versionId,
                                      QString *messageOut);

    bool storeManagedRaw(const QString &sourcePath, const QString &assetId,
                         const QString &versionId, QString *relPathOut, QString *shaOut,
                         QString *error);

    // dedup 补挂（§3）：同一 SHA-256 再导入时，按原导入的井名解析顺序重试
    // asset 的未决链接——现在恰好匹配一口井的挂上去（同名仍多候选/零匹配不动）。
    // 返回补挂条数。
    int attachResolvableLinks(const CatalogAsset &asset, const QString &sourcePath,
                              QString *error = nullptr);

    // 文档 PDF 转换：LibreOffice 单实例在共享 UserInstallation 下不可靠，
    // 一律串行（队列）。soffice 把输出写到 --outdir/<stem>.pdf。
    void resolveDocumentConverter();
    void startNextDocumentPdf();
    void finishDocumentPdf(int exitCode);

    PaleoProjectStore *m_store;
    QString m_projectDir;
    DataCatalog *m_catalog = nullptr;
    bool m_catalogReady = false;
    QString m_catalogOpenError;   // 最近一次 catalog open 失败原因（成功则空）

    QString m_converter;            // "" 未解析/不可用
    bool m_converterResolved = false;
    QProcess *m_pdfProc = nullptr;  // 非空即转换在途
    QStringList m_pdfQueue;
    QString m_pdfCurrent;           // 在途 assetId
    QString m_pdfCurrentVersionId;
    QString m_pdfRawVersionId;
    QString m_pdfOutFile;           // 期望产物绝对路径
    QSet<QString> m_pdfPending;
    QHash<QString, QString> m_pdfErrors;
};
