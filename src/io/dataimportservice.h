#pragma once
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>

class QgisLayerService;
class PaleoProjectStore;
class DataCatalog;
class QProcess;
struct CatalogVersion;

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
    };

    DataImportService(QgisLayerService *layers, PaleoProjectStore *store, QObject *parent = nullptr);
    ~DataImportService() override;

    void setProjectDir(const QString &dir);   // where the project lives

    // 新契约入口。返回资产 id；失败返回空串并设置 *error。
    QString importProjectFile(const QString &sourcePath, QString *error = nullptr);
    QString importProjectFile(const QString &sourcePath, const ImportOptions &options,
                              QString *error = nullptr);

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
    void documentPdfReady(const QString &assetId);
    void documentPdfFailed(const QString &assetId, const QString &error);

  private:
    struct WellBind
    {
      QString entityId;   // 恰好一个匹配
      bool unresolved = false;
      QStringList candidates; // 0/2+ 候选时的集合
    };
    WellBind resolveWell(const QString &name) const;

    bool storeManagedRaw(const QString &sourcePath, const QString &assetId,
                         const QString &versionId, QString *relPathOut, QString *shaOut,
                         QString *error);

    // 文档 PDF 转换：LibreOffice 单实例在共享 UserInstallation 下不可靠，
    // 一律串行（队列）。soffice 把输出写到 --outdir/<stem>.pdf。
    void resolveDocumentConverter();
    void startNextDocumentPdf();
    void finishDocumentPdf(int exitCode);

    QgisLayerService *m_layers;
    PaleoProjectStore *m_store;
    QString m_projectDir;
    DataCatalog *m_catalog = nullptr;

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
