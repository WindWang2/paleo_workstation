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

class QgisLayerService;
class PaleoProjectStore;
class QProcess;

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

    DataImportService(QgisLayerService *layers, PaleoProjectStore *store, QObject *parent = nullptr);
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
    struct FolderRowResult
    {
      QString path;            // 源路径（所选目录内）
      QString classifiedType;  // 分类器类型（well_head/well_log/tops/...）
      QString entityName;      // 已解析实体名；多个主关联用 ", " 连接；未决/失败为空
      enum class Outcome { Imported, Unresolved, Failed, Skipped };
      Outcome outcome = Outcome::Skipped;
      QString message;         // 失败原因 / 未决备注 / dedup「字节已在库」文案
    };
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

    // 单行重导（确认表「重试」）：forceType 口径同 typeOverrides 的单值
    // （空=按分类器；非法值忽略）。返回与 importFolder 相同口径的行结果；
    // 失败时 *error 带行 message。
    FolderRowResult importFolderRow(const QString &sourcePath,
                                    const QString &forceType = QString(),
                                    QString *error = nullptr);

    // 确认表预览：与 importFolder 同一枚举/分类口径，只列行不导入。
    // skipped=true 的行是软链逃逸/非普通文件（预览里灰显、不可改类型）。
    struct FolderPreviewRow
    {
      QString path;
      QString classifiedType;
      bool skipped = false;
      QString skipReason;
    };
    QVector<FolderPreviewRow> previewFolder(const QString &dirPath, QString *error = nullptr);

    // 旧签名（mainwindow importRequested 接线）：kind 仅用于信号，不再决定行为。
    QString importFile(const QString &kind, const QString &sourcePath, QString *error = nullptr);

    DataCatalog *catalog() const { return m_catalog; }

    // 版本路径 → 工程内绝对路径（外链原样返回）。
    QString absolutePath(const QString &assetId) const;
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

  signals:
    void imported(const QString &kind, const QString &assetId, const QString &layerId);
    void importFailed(const QString &kind, const QString &path, const QString &error);
    // setProjectDir 里 catalog open 失败即发；成功打开后 catalogOpenError() 清空。
    void catalogOpenFailed(const QString &error);
    void documentPdfReady(const QString &assetId);
    void documentPdfFailed(const QString &assetId, const QString &error);

  private:
    // 线程规则（D1b/D1c 异步导入）：import* 系列可在 worker 线程执行——内部
    // 对 catalog 的每一次读写经 catInvoke marshal 回 catalog 所在线程（GUI）。
    // GUI 线程调用 = 直调零成本；worker 线程 = BlockingQueuedConnection 排队
    // 执行并等结果。catalog 因此永远只被它自己的线程触碰——GUI 侧其他调用点
    // 不用改。对 m_layers（同在 GUI 线程）的 declare 也走它。
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
        QMetaObject::invokeMethod(m_catalog, std::forward<Fn>(fn),
                                  Qt::BlockingQueuedConnection);
      else
      {
        R result{};
        QMetaObject::invokeMethod(m_catalog, [&result, &fn] { result = fn(); },
                                  Qt::BlockingQueuedConnection);
        return result;
      }
    }

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

    QgisLayerService *m_layers;
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
