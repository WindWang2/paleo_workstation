#pragma once
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

class QgisLayerService;
class PaleoProjectStore;
class DataCatalog;

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

    // ---- 兼容面（bottom-dock 面板仍在用）----
    QStringList assets(const QString &type = QString()) const;   // asset ids（按分类类型过滤）
    QString assetSource(const QString &assetId) const;           // 当前版本路径（外链绝对）

  signals:
    void imported(const QString &kind, const QString &assetId, const QString &layerId);
    void importFailed(const QString &kind, const QString &path, const QString &error);

  private:
    struct WellBind
    {
      QString entityId;   // 恰好一个匹配
      bool unresolved = false;
      QStringList candidates; // 0/2+ 候选时的集合
    };
    WellBind resolveWell(const QString &name) const;

    bool ensureAuxUnresolved(QString *entityId, QString *error);
    bool storeManagedRaw(const QString &sourcePath, const QString &assetId,
                         const QString &versionId, QString *relPathOut, QString *shaOut,
                         QString *error);

    QgisLayerService *m_layers;
    PaleoProjectStore *m_store;
    QString m_projectDir;
    DataCatalog *m_catalog = nullptr;
};
