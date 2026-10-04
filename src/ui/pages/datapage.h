// 层：视图
#pragma once

#include <QPointer>
#include <QWidget>
#include <QString>
#include <QVariantMap>

#include "entitypanel.h"

class DataListPanel;
class PreviewDocService;

// ui/pages/datapage — 数据管理兼容薄壳（W5 分家后）：内部由
// DataListPanel（导入+列表）与 EntityPanel（实体数据视图）组成，公共
// API/objectName/信号签名全部不变，壳与测试零改动。
//
// 服务经动态属性 "paleo.page.importsvc"（QObject* → PreviewDocService，
// 数据页唯一数据门面）绑定；本壳经 DynamicPropertyChange 下发给两个子
// 面板。"paleo.page.entityId"/"paleo.page.assetId" 记录实体视图上下文。
class DataPage : public QWidget
{
  Q_OBJECT
  public:
    explicit DataPage(QWidget *parent = nullptr);
    // 实体数据视图段——壳可把它重新挂到别处（EntityPanel 本体）。
    QWidget *entityViewSection() const { return m_entityPanel; }

    // ---- P3 D6：命令面板/快捷键面（壳与测试入口）----
    void openCommandPalette();                    // Ctrl+Shift+P
    void openShortcutsDialog();                   // ? 键
    bool vimModeEnabled() const;
    void setVimModeEnabled(bool on);
    // B2（wave/deepen-perf）：列表面板露出——壳接导入队列生产 runner 用。
    DataListPanel *listPanel() const { return m_listPanel; }

  public slots:
    void refreshAssetTable();                     // 资产表/树 + 实体视图一并重取
    void refreshEntityView();                     // 实体角色槽视图重取
    void selectAssetsForEntities(const QStringList &entityIds); // D6 地图→表联动
    void setUnresolvedFilter(bool on);            // T31「查看未决」过滤
    void applyListFilter();                       // 搜索/类型筛选
    void focusVersion(const QString &assetId, const QString &versionId);
    void selectAsset(const QString &assetId);     // 选中资产 + 实体视图定位

  signals:
    void importRequested(const QString &kind);  // "wells" | "seismic" | "boundary" | ...
    void versionActivated(const QString &versionId);
    void assetActivated(const QString &assetId); // 列表选中 → 预览标签打开
    // 树节点关联井选中 → 预览打开并定位到该井
    void assetWellActivated(const QString &assetId, const QString &wellId);
    void seismicLineActivated(const QString &assetId, const QString &mode); // 测线激活
    void wellSelected(const QString &wellId); // 树中选中井 → 地图高亮
    void surveyAreaActivated(); // 双击测区 → 打开测区全景地图
    // ---- P3 新增 ----
    void statusMessage(const QString &msg);        // D5.4/D8（壳接状态栏）
    void externalImportRequested(const QStringList &paths); // D3.2（壳接导入流）

  protected:
    bool event(QEvent *event) override; // 动态属性变更 → 门面下发子面板

  private:
    PreviewDocService *docService() const;
    void wireDataOps();      // P3：dataops 信号/共享栈接线（幂等）
    bool m_dataopsWired = false;

    DataListPanel *m_listPanel = nullptr;
    EntityPanel *m_entityPanel = nullptr;
};
