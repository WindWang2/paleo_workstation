// 层：视图
#pragma once

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

class PreviewDocService;
class QTreeWidget;
class QStackedWidget;

// ui/pages/datalist — 数据管理页的「导入 + 列表」侧（W5：自 DataPage 分家）。
//
// 含两段（objectName 不变）：dataImportSection（导入按钮）/ dataListSection
//（搜索 + 类型筛选 + 未决过滤条 + 树/表 viewStack）。数据访问只走
// PreviewDocService 门面（壳经 DataPage 动态属性下发 setDocService）。
// 信号签名与旧 DataPage 一致（薄壳转发到壳）。
class DataListPanel : public QWidget
{
  Q_OBJECT

  public:
    explicit DataListPanel(QWidget *parent = nullptr);

    // 门面下发（DataPage 的 "paleo.page.importsvc" 动态属性 → 本面板）。
    void setDocService(PreviewDocService *doc);

  public slots:
    void refreshAssetTable();   // 从 catalog 资产重建资产表/树
    void applyListFilter();     // 名称/类型/关联含搜索词且类型匹配的行才显示
    // T31「查看未决」：资产表过滤到仍有未决链接的行；off 清除过滤。
    void setUnresolvedFilter(bool on);
    // D6 地图→表联动：实体 id 集合 → 选中这些实体已决关联对应的资产行
    // （滚动到首个命中行；行选中照发 assetActivated）。无命中不改当前选中。
    void selectAssetsForEntities(const QStringList &entityIds);
    // 表 + 树定位到指定资产（不发激活信号——setCurrentCell 走 QSignalBlocker）。
    void selectAssetInViews(const QString &assetId);

  signals:
    void importRequested(const QString &kind);   // "wells" | "seismic" | "boundary" | ...
    void assetActivated(const QString &assetId); // 列表选中 → 预览标签打开
    // 树节点关联井选中 → 预览打开并定位到该井
    void assetWellActivated(const QString &assetId, const QString &wellId);
    void seismicLineActivated(const QString &assetId, const QString &mode);
    void wellSelected(const QString &wellId); // 树中选中井 → 地图高亮
    void surveyAreaActivated();               // 双击测区 → 测区全景地图
    // 内部建/撤挂接变更 catalog 后请实体视图重取（薄壳接 EntityPanel::refresh）。
    void entityRefreshRequested();
    // 列表/树内选中资产或实体——薄壳转回 DataPage::selectAsset /
    // selectAssetsForEntities（实体视图定位口径由壳统一）。
    void assetFocusRequested(const QString &assetId);
    void entitiesFocusRequested(const QStringList &entityIds);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void refreshAssetTree();

    PreviewDocService *m_doc = nullptr;
    QTreeWidget *m_tree = nullptr;
    QStackedWidget *m_viewStack = nullptr;
};
