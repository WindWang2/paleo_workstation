#pragma once
#include <QHash>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <memory>

class DataImportService;
class PaleoTaskService;
class PaleoTask;
class QLabel;
class QTabWidget;
struct CatalogAsset;
class SegyReader;

// ui/datapreview — 数据页页内预览标签栏（docs/PROJECT_AREA_PLAN.md §4）。
// 普通 QTabWidget（可关闭标签），样式走 DESIGN.md dock 面板，不用工作流
// 标签的蓝色下划线。预览只出现在数据管理页，不泄漏到其他页。
//
// 状态文案（§4）：
//   空态   「还没有打开的预览 — 在列表中选择一条数据」
//   读取中 「正在读取」+文件名（同步读取前置状态，不做线程）
//   失败   「读取失败」+原因+文件名+「重试」；外链缺失 「找不到源文件」+路径
// 标题：「文件名 · 井名」/「文件名 · 测线」；无过滤时只有文件名。
// 多井资产（井口表、DC.dat、TD）每标签自带「井」下拉框，只列已决链接的井；
// 未选井时正文是「先选择一口井」。重选已开资产聚焦已有标签，不改它已选的井。
class DataPreviewTabs : public QWidget
{
  Q_OBJECT
  public:
    explicit DataPreviewTabs(QWidget *parent = nullptr);

    // 服务绑定（mainwindow 接线处调用；为空时 openAsset 显示空态）。
    void setImportService(DataImportService *svc);

    // 任务服务（wave2 D1）：接上后地震标签的「索引+SHA 复验+测线解码」进
    // 任务池异步执行（任务页有进度条/ETA/取消）；不接线保持同步旧行为
    //（小夹具测试环境用）。异步模式下索引按 assetId 缓存——换测线只重解码
    // 该线，不再整文件重建（T23）。
    void setTaskService(PaleoTaskService *svc);

    // 从资产列表选中一条资产：已有标签则聚焦，否则新开一个可关闭标签。
    void openAsset(const QString &assetId);

    // 验证页「在数据页看这条剖面」（预览壳重排 §4）：打开/聚焦地震资产
    // 标签并把测线控件（lineMode/lineSpin）拨到 kind+line——控件自己的
    // decode 链路负责换测线，不在此处解析。kind: "inline"|"crossline"。
    // timeMs 当前只透传记录——剖面已有的 D61 标定线就是同一时间轴标注，
    // 不在剖面上另画第二条线。非地震资产/无测线控件时静默返回。
    void openSeismicLine(const QString &assetId, const QString &kind,
                        int line, double timeMs);

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
    QWidget *buildContent(const QString &assetId, QWidget *page);
    void rebuildAssetTab(const QString &assetId); // 「重试」/PDF 转换完成后重建内容
    void focusWellIfNeeded(const QString &assetId, QWidget *page);
    void updateTabTitle(const QString &assetId); // 文件名 + 过滤后缀（井/测线）
    // 井过滤型正文（well_head / well_stratification / time_depth）：
    // wellName 为空 → 「先选择一口井」占位；否则按该井过滤渲染。
    QWidget *buildWellBody(const CatalogAsset &asset, const QString &absPath,
                           const QString &wellEntityId, const QString &wellName,
                           QWidget *parent);
    // 「读取失败」+原因+文件名+「重试」（重试=重建该标签）。
    QWidget *failureState(const QString &assetId, const QString &reason, QWidget *parent);
    // 「正在读取」+文件名 标签（同步读取前置；buildContent 完成后隐藏）。
    QLabel *loadingLabel(const QString &fileName, QWidget *parent);

    DataImportService *m_svc = nullptr;
    PaleoTaskService *m_taskSvc = nullptr;
    QTabWidget *m_tabs = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QHash<QString, QWidget *> m_pageOfAsset;
    QHash<QString, QString> m_wellEntityOfAsset; // assetId → 该标签已选井（多井下拉框）
    QHash<QString, QString> m_titleSuffixOfAsset; // assetId → 「 · 井名」/「 · IL1315」
    // D1 异步解码：按资产的索引缓存（每资产只 open/SHA 一次）、世代号
    //（陈旧结果丢弃）、进行中任务指针（新解码请求取消旧任务）。
    QHash<QString, std::shared_ptr<SegyReader>> m_segyReaders;
    QHash<QString, int> m_decodeSeq;
    QHash<QString, QPointer<PaleoTask>> m_decodeTask;
    QHash<QString, bool> m_shaVerified; // assetId → 本会话已过 SHA 复验（不重复哈希）
};
