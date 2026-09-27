#pragma once
#include <QPointer>
#include <QWidget>
#include <QString>
#include <QVariantMap>

// ui/pages/ — per-page right-dock panels (§42.2 inventory).
// Each panel is a plain QWidget bound to services/workflows via ctor injection.
// UI never touches Qgs* classes directly (§25) — panels emit intents; the
// shell/workflows do the GIS work. objectName set on every interactive widget
// for tests.
//
// m2/mapping-pages：预测/约束单因素/综合编图三页拆至各自文件（公共形状
// 不变，本头 include 之，既有 include 点不受影响）：
#include "predictpage.h"
#include "constraintpage.h"
#include "composepage.h"

class ValidationWorkflow;
class QgisLayerService;
class PaleoOnnxService;

// 数据管理 — asset import intents + asset table（§4 预览壳重排：页内预览
// 已挪到主区地图下方的竖向分栏，右 dock 只留导入按钮与资产表）。
// 服务经动态属性 "paleo.page.importsvc"（QObject* → DataImportService）绑定，
// 由 mainwindow 接线处设置；catalog 变更后调用 refreshAssetTable() 刷新列表。
//
// 三段（objectName）：dataImportSection（导入按钮）/ dataListSection（搜索 +
// 类型筛选 + 计数 + 资产表）/ entityViewSection（实体数据视图）。ribbon 壳
// 把本页放进中央「数据列表」、实体段挪进右侧「数据属性」dock、导入段藏起
// （命令在 ribbon）；单独使用时三段竖排。
class DataPage : public QWidget
{
  Q_OBJECT
  public:
    explicit DataPage(QWidget *parent = nullptr);
    // 实体数据视图段——壳可把它重新挂到别处（刷新按段内 objectName 查找，
    // 与挂在哪无关）。
    QWidget *entityViewSection() const { return m_entitySection; }
  public slots:
    void refreshAssetTable();                     // 从 catalog 资产重建资产表
    // p5a（data/view-wiring）：当前选中实体的角色槽数据视图重取——
    // entityDataView()（B 包纯查询门面）按角色词表枚举 (实体,角色) 槽、
    // 下游 DERIVED 产物与悬空血缘诊断。当前实体由 selectAssetsForEntities
    // 记录（D6 地图点选通路）；catalog.changed() → refreshAssetTable() 的
    // 主窗口接线里一并重取（纯查询，不写 catalog）。
    void refreshEntityView();
    // D6 地图→表联动：实体 id 集合（画布点选井）→ 选中这些实体已决关联
    // 对应的资产行（滚动到首个命中行）。行选中照走 assetActivated → 预览
    // 打开，与手点同一通路；未决链接不算命中。无命中不改当前选中。
    // 首个选中 id 同时驱动实体角色槽视图（p5a）。
    void selectAssetsForEntities(const QStringList &entityIds);
    // T31「查看未决」：把资产表过滤到仍有未决链接的行；off 清除过滤。
    void setUnresolvedFilter(bool on);
    // 列表面筛选：名称/类型/关联含搜索词（不分大小写）且类型匹配下拉的行
    // 才显示；计数标签写「共 N 条」或「显示 M / 共 N 条」。刷新后自动重放。
    void applyListFilter();
    // 选中指定资产并在右侧属性面板显示其详细属性
    void selectAsset(const QString &assetId);
  signals:
    void importRequested(const QString &kind);  // "wells" | "seismic" | "boundary"
    void assetActivated(const QString &assetId); // 列表选中 → 预览标签打开
    void assetWellActivated(const QString &assetId, const QString &wellId); // 树节点关联井选中 → 预览打开并定位到该井
    void seismicLineActivated(const QString &assetId, const QString &mode); // 测线激活
    void wellSelected(const QString &wellId); // 树中选中井 → 地图高亮

  private:
    void refreshAssetTree();
    QPointer<QWidget> m_entitySection;
    class QTreeWidget *m_tree = nullptr;
    class QStackedWidget *m_viewStack = nullptr;
};

class QShowEvent;

// ④验证 — run button + issues table + locate intent.
class ValidatePage : public QWidget
{
  Q_OBJECT
  public:
    ValidatePage(ValidationWorkflow *wf, QWidget *parent = nullptr);
    void populate();                              // run validate(), fill table
    // 残差表渲染（populate 复用）：行 map 契约 well_name/status/residual_ms/
    // reason/threshold_ms…。独立成静态面供测试直灌（workflow 无注入点时
    // validate() 会清空 residualRows 属性）。
    static void fillResidualTable(class QTableWidget *table, const QVariantList &rows);
  signals:
    // wave/mapping-pipeline：payload 携带三视图联动所需的机器字段（来自
    // ValidationIssue::wellId + details）：wellId、horizon、inline、time_ms
    // 等；非残差问题 payload 为空表。layerId 仍用于地图缩放。
    void locateRequested(const QString &layerId, const QString &wktLocation,
                         const QVariantMap &payload);
    // 「在数据页看这条剖面」（预览壳重排）：payload 同 locateRequested——
    // inline 是目标测线号、time_ms 是目标时间。shell 负责换页+打开剖面标签。
    void seismicSectionRequested(const QVariantMap &payload);
};
