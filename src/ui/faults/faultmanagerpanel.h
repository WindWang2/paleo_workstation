// 层：视图
#pragma once
#include <QPointer>
#include <QWidget>

#include "domain/faultset.h"

class QgisCanvasController;
class QComboBox;
class QLabel;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QgsAdvancedDigitizingDockWidget;
class QgsMapTool;

namespace paleo::fault {

class FaultInterpretationController;

// ui/faults/faultmanagerpanel — 断层管理面板（goal/fault-interpretation）。
// 视图只发信号：树/按钮把意图全部转给 FaultInterpretationController
// （编排器改模型+落盘+广播），面板自身只做展示与选中态镜像。
//
// 能力：命名断层树（显隐勾选/改名/删除）、断层棒与层位切割子节点、
// 撤销/重做镜像按钮、层位图切割多边形绘制（PaleoDrawPolygonTool 复用，
// 落 controller.setCut）、上盘方向设置、SelectionContext 三视图联动高亮。
class FaultManagerPanel : public QWidget
{
    Q_OBJECT
public:
    FaultManagerPanel(FaultInterpretationController *controller,
                      QgisCanvasController *canvasCtl, QWidget *parent = nullptr);

    // 当前树中选中的断层（无 → 空串）
    QString selectedFaultId() const;

signals:
    // 绘制开关状态变化（壳层可据此同步 ribbon 动作）
    void cutDrawToggled(bool active);

private slots:
    void onAddFault();
    void onRenameFault();
    void onRemoveFault();
    void onToggleCutDraw(bool checked);
    void onCutDrawn(const QString &wkt);
    void onCutDrawAborted();
    void onSelectionChanged();
    void onItemChanged(QTreeWidgetItem *item, int column);
    void onHangingSideChanged();
    void onFaultSetChanged();
    void onFaultSelectionChanged(const QStringList &faultIds);

private:
    void buildUi();
    void refreshTree();
    void teardownCutTool();
    QTreeWidgetItem *itemForFault(const QString &faultId) const;

    FaultInterpretationController *m_controller = nullptr;
    QgisCanvasController *m_canvasCtl = nullptr;
    QToolButton *m_btnAdd = nullptr;
    QToolButton *m_btnRename = nullptr;
    QToolButton *m_btnRemove = nullptr;
    QToolButton *m_btnUndo = nullptr;
    QToolButton *m_btnRedo = nullptr;
    QToolButton *m_btnDrawCut = nullptr;
    QTreeWidget *m_tree = nullptr;
    QComboBox *m_cboHangingSide = nullptr;
    QLabel *m_lblStatus = nullptr;
    QPointer<QgsMapTool> m_cutTool; // 层位图切割绘制工具（激活期间）
    QPointer<QgsAdvancedDigitizingDockWidget> m_cadDock; // 工具的 CAD dock（canvas 生命周期）
    bool mRefreshing = false;      // 程序化刷新期间屏蔽树信号
    bool mSyncingSelection = false; // 联动回声期间屏蔽选中信号
};

} // namespace paleo::fault
