// 层：视图
#pragma once

#include <QString>
#include <QWidget>

class PreviewDocService;

// ui/pages/entitypanel — 数据管理页的「实体数据视图」侧（W5：自 DataPage
// 分家）。本面板自身即 entityViewSection（objectName 保留——壳把它重新
// 挂到右侧「数据属性」dock）。
//
// p5a：当前选中实体的角色槽数据视图——entityDataView()（B 包纯查询门面）
// 按角色词表枚举 (实体,角色) 槽、下游 DERIVED 产物与悬空血缘诊断。
// 上下文由壳/列表侧经 setContext 下发（D6 地图点选 → entityId；表选中 →
// assetId 反推实体）。catalog.changed() 由主窗口接线调 refresh() 重取。
class EntityPanel : public QWidget
{
  Q_OBJECT

  public:
    explicit EntityPanel(QWidget *parent = nullptr);

    void setDocService(PreviewDocService *doc);
    void setContext(const QString &entityId, const QString &assetId);

  public slots:
    void refresh(); // 重取并重渲染角色槽/派生/属性段

  private:
    PreviewDocService *m_doc = nullptr;
    QString m_entityId;
    QString m_assetId;
};
