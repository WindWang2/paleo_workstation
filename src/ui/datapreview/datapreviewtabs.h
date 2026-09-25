#pragma once
#include <QHash>
#include <QString>
#include <QWidget>

class DataImportService;
class QLabel;
class QTabWidget;

// ui/datapreview — 数据页页内预览标签栏（docs/PROJECT_AREA_PLAN.md §4）。
// 普通 QTabWidget（可关闭标签），样式走 DESIGN.md dock 面板，不用工作流
// 标签的蓝色下划线。预览只出现在数据管理页，不泄漏到其他页。
//
// 状态文案（§4）：
//   空态   「还没有打开的预览 — 在列表中选择一条数据」
//   读取中 「正在读取」+文件名
//   失败   原因+文件名；外链缺失 「找不到源文件」+路径
// 重选已开资产聚焦已有标签；切换不丢内容（每资产一个常驻页）。
class DataPreviewTabs : public QWidget
{
  Q_OBJECT
  public:
    explicit DataPreviewTabs(QWidget *parent = nullptr);

    // 服务绑定（mainwindow 接线处调用；为空时 openAsset 显示空态）。
    void setImportService(DataImportService *svc);

    // 从资产列表选中一条资产：已有标签则聚焦，否则新开一个可关闭标签。
    void openAsset(const QString &assetId);

    int tabCount() const;
    QString assetIdAt(int index) const;   // "" 越界
    void closeAssetTab(const QString &assetId);
    bool isMissingSourceState(const QString &assetId) const; // 外链缺失态（测试/诊断）

  signals:
    // well_head 标签被选中/聚焦时，地图高亮该井（§4）。
    void wellSelected(const QString &wellEntityId);
    // horizon 标签「在地图上显示」按钮（§4）。
    void showHorizonOnMapRequested(const QString &layerId);

  private:
    QWidget *buildContent(const QString &assetId, QString *titleOut, QString *wellEntityOut,
                          QString *horizonLayerOut);
    void focusWellIfNeeded(const QString &assetId, QWidget *page);

    DataImportService *m_svc = nullptr;
    QTabWidget *m_tabs = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QHash<QString, QWidget *> m_pageOfAsset;
};
