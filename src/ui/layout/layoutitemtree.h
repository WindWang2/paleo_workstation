// 层：视图
#pragma once
#include <QPointer>
#include <QWidget>

class QTreeView;
class QgsLayout;
class QgsLayoutItem;

// ui/layout/layoutitemtree — 元素树面板（方向 25 M6）。
//
// QGIS 原生路径：QgsLayout::itemsModel()（QgsLayoutModel：可见性/锁定/项 id
// 三列，与 QGIS 设计器「Items 面板」同一模型）直接挂 QTreeView——不自建
// 树、不复制状态（DESIGN.md：能复用的 QGIS 控件直接用）。
//
// 本类只做三件宿主的事：
//   1. attach(layout) 换模型（null 版面 = 空树不崩）；
//   2. 树里选中 → emit itemActivated(item)（壳把它转成版面选中）；
//   3. doubleClicked → emit itemShowOptions(item)（壳打开右侧属性面板）。
// 外部选中变化（视图/别的面板）由壳调 highlightItem 同步树的高亮。
// 生命周期：模型归 layout 所有；树里的 QgsLayoutItem* 经 itemFromIndex 取，
// 只作信号载荷——接收方自行守悬挂（壳/面板已有 QPointer 纪律）。
class PaleoLayoutItemTree : public QWidget
{
    Q_OBJECT

  public:
    explicit PaleoLayoutItemTree( QWidget *parent = nullptr );

    //! 绑定版面（换模型）；nullptr = 空树。可重复调用。
    void attach( QgsLayout *layout );

    //! 当前树中选中的项（无选中/无版面 = nullptr）。
    QgsLayoutItem *currentItem() const;

    //! 外部选中同步：高亮对应行（无匹配 = 清高亮）。不发信号（防回环）。
    void highlightItem( QgsLayoutItem *item );

  signals:
    void itemActivated( QgsLayoutItem *item );     //!< 树内点击/键盘选中
    void itemShowOptions( QgsLayoutItem *item );   //!< 双击 → 请求属性面板

  private:
    QTreeView *m_view = nullptr;
    QPointer<QgsLayout> m_layout;
    bool m_syncing = false; // highlightItem 期间抑制 itemActivated 回环
};
