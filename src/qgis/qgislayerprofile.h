// 层：QGIS 封装
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QgsLayerTreeModel;
class QgsLayoutItemMap;
class QgsMapThemeCollection;
class QgsProject;
class QgisLayerService;

// 页面档案服务 ——「不同页面激活不同图层」的核心。
//
// 底部机制 = QgsMapThemeCollection（挂在 QgsProject::mapThemeCollection()，
// 随 .qgz 持久化）：页面档案与用户命名主题共用同一存储，页面主题名带
// "page:" 前缀。本服务不持有任何可见性台账（PALEO_QGIS_PLAN §5：
// QgsLayerTree/主题是唯一图层可见性状态源）。
//
// 档案应用语义：档案表内组的图层置可见、表外组置隐藏（纯可见性主题，
// 不删图层、不动 manifest）；用户手改可见性后档案不覆盖，除非显式再调
// applyPageProfile。data 页不操作画布（无地图）。
//
// 组匹配 = 声明驱动：树里图层经 paleoLayerId 自定义属性回查 manifest
// declaration.group；同名树组（存在时）整组一并纳入。落在树根、无
// declaration 的临时图层（用户手加）不动其可见性。旧组名词表
//（01_Prediction/02_Constraints/03_Predict/03_Composite/00_Data——
// workflows.cpp 历史值）不在档案表内 → 表外处理（记 TODOS 迁移）。
//
// 与 QgisLayerService::setActiveHorizon 的时序：先换层位（实例化/释放）
// 再应用页面主题；apply* 对主题记录里已不在工程中的 layerId 做防御性
// 修剪，防主题应用到刚释放的图层。
class QgisLayerProfileService : public QObject
{
  Q_OBJECT

  public:
    explicit QgisLayerProfileService(QgsProject *project, QObject *parent = nullptr);
    ~QgisLayerProfileService() override;

    // 可见性落在模型上：apply/capture 前必须注入图层树模型
    //（壳接线时取 LayerTreePanel::layerTreeModel()）。
    void setLayerTreeModel(QgsLayerTreeModel *model);
    QgsLayerTreeModel *layerTreeModel() const { return m_model; }

    // 可选注入（predict 档案的「当前层位约束图层」需要 activeHorizon +
    // declarations；未注入则 predict 只按组表）。
    void setLayerService(QgisLayerService *service);
    QgisLayerService *layerService() const { return m_layerService; }

    // 内置页面档案表（默认组集合，词表见 layermanifest group 七值）：
    //   predict   → 01_Base, 02_Prediction
    //   constraint→ 01_Base, 03_Constraints, 04_SingleFactor
    //   compose   → 01_Base, 03_Constraints, 04_SingleFactor, 05_PaleoMap, 06_Reference
    //   validate  → 01_Base, 07_Validation
    //   data      → 空（数据页无地图，不操作画布）
    // predict 额外含「当前层位约束图层」（03_Constraints 内属于
    // QgisLayerService::activeHorizon() 的声明层）——由 applyPageProfile
    // 生成主题时并入。
    static QStringList defaultProfileGroups(const QString &pageId);
    // 档案表扩展覆盖：按 pageId 替换默认组集合（manifest 允许扩展）。
    void setProfileGroupsOverride(const QString &pageId, const QStringList &groups);
    QStringList profileGroupsFor(const QString &pageId) const;

    // 应用页面档案：主题不存在时先按档案表摆树可见性，再
    // createThemeFromCurrentState 生成 "page:<pageId>" 记录 insert 进
    // collection；已存在则直接 applyTheme。记录当前页（供换层位后重放）。
    bool applyPageProfile(const QString &pageId);
    // 换层位后重放当前页档案（时序兜底：层位先换完，再重放）。
    bool applyCurrentPageProfile();
    QString currentPageProfile() const { return m_currentPage; }

    // 页面档案主题名约定。
    static QString pageThemeName(const QString &pageId);

    // 主题生命周期（collection 直通）。captureCurrentAsTheme 空名失败。
    bool captureCurrentAsTheme(const QString &name);
    bool applyTheme(const QString &name);
    bool removeMapTheme(const QString &name);
    bool hasTheme(const QString &name) const;
    QStringList themes() const; // 全部主题（含 page:*）

    // 打印布局联动：版面地图项钉可见性主题（setFollowVisibilityPreset +
    // setFollowVisibilityPresetName；智能编图导出版面可钉 compose 主题）。
    void setLayoutMapTheme(QgsLayoutItemMap *mapItem, const QString &themeName);

  signals:
    void profileApplied(const QString &pageId, const QString &themeName);
    // 转发 QgsMapThemeCollection::mapThemesChanged
    void mapThemesChanged();

  private:
    // 工程的主题集合（m_project 为 null 时 nullptr）。
    QgsMapThemeCollection *themeCollection() const;
    // 应用一个已存在的主题：先防御性剔除记录里已不在工程中的图层
    //（层位切换释放实例的悬空记录），update 回写后再 applyTheme。
    // 修剪后记录为空仍可应用（全隐藏语义）。
    bool applyPrunedTheme(const QString &name);
    // 按档案表「摆树」：声明驱动的组匹配（paleoLayerId → declaration.group），
    // predict 页并入当前层位约束图层，同名树组按组内结果同步勾选态。
    // layerService 为 null 或清单读失败 → 不动任何可见性（读失败时返回 false）。
    bool stageTreeVisibility(const QStringList &groups, bool mergeActiveHorizonConstraints);

    QgsProject *m_project = nullptr;
    QgsLayerTreeModel *m_model = nullptr;
    QgisLayerService *m_layerService = nullptr;
    QHash<QString, QStringList> m_groupOverrides;
    QString m_currentPage;
};
