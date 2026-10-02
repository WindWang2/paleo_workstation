// 层：视图
#include "faultmanagerpanel.h"

#include "qgis/qgiscanvascontroller.h"
#include "ui/maptools/paleomaptools.h"
#include "ui/paleotheme.h"
#include "workflow/faultinterpretationcontroller.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QTreeWidgetItemIterator>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <qgsadvanceddigitizingdockwidget.h>
#include <qgsmapcanvas.h>
#include <qgsmaptool.h>

namespace paleo::fault {

namespace {
// 与 SeismicPickPanel 同款按钮 chrome（token 化、随主题重算）。
QToolButton *mkBtn(const QString &text, const QString &tooltip)
{
    auto *btn = new QToolButton();
    btn->setText(text);
    btn->setToolTip(tooltip);
    PaleoTheme::applyThemedStyleSheet(btn, [] { return PaleoTheme::toolButtonStyleSheet(); });
    return btn;
}

QString hangingSideLabel(FaultHangingSide side)
{
    switch (side) {
    case FaultHangingSide::Left:
        return QObject::tr("上盘: 左");
    case FaultHangingSide::Right:
        return QObject::tr("上盘: 右");
    default:
        return QObject::tr("上盘: 未定");
    }
}
} // namespace

FaultManagerPanel::FaultManagerPanel(FaultInterpretationController *controller,
                                     QgisCanvasController *canvasCtl, QWidget *parent)
    : QWidget(parent), m_controller(controller), m_canvasCtl(canvasCtl)
{
    buildUi();
    if (m_controller) {
        connect(m_controller, &FaultInterpretationController::faultSetChanged, this,
                &FaultManagerPanel::onFaultSetChanged);
        connect(m_controller, &FaultInterpretationController::faultSelectionChanged, this,
                &FaultManagerPanel::onFaultSelectionChanged);
        if (auto *stack = m_controller->editStack()) {
            connect(stack, &FaultEditStack::canUndoChanged, m_btnUndo, &QToolButton::setEnabled);
            connect(stack, &FaultEditStack::canRedoChanged, m_btnRedo, &QToolButton::setEnabled);
            m_btnUndo->setEnabled(stack->canUndo());
            m_btnRedo->setEnabled(stack->canRedo());
        }
    }
    refreshTree();
}

QString FaultManagerPanel::selectedFaultId() const
{
    // 选中项优先（子节点向上归到断层行），当前项兜底——两种事件时序
    // （setSelected 先于 setCurrentItem）下都解析到断层。
    const QList<QTreeWidgetItem *> selected = m_tree->selectedItems();
    for (QTreeWidgetItem *item : selected) {
        while (item) {
            const QString id = item->data(0, Qt::UserRole).toString();
            if (!id.isEmpty())
                return id;
            item = item->parent();
        }
    }
    QTreeWidgetItem *item = m_tree->currentItem();
    while (item) {
        const QString id = item->data(0, Qt::UserRole).toString();
        if (!id.isEmpty())
            return id;
        item = item->parent();
    }
    return QString();
}

void FaultManagerPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto *bar = new QHBoxLayout();
    bar->setSpacing(4);
    m_btnAdd = mkBtn(tr("+ 断层"), tr("新建命名断层"));
    m_btnRename = mkBtn(tr("改名"), tr("重命名选中断层"));
    m_btnRemove = mkBtn(tr("删除"), tr("删除断层及其全部断层棒/切割（可撤销）"));
    m_btnUndo = mkBtn(tr("撤销"), tr("撤销最近一次断层编辑"));
    m_btnRedo = mkBtn(tr("重做"), tr("重做被撤销的断层编辑"));
    m_btnDrawCut = mkBtn(tr("画切割"), tr("在层位平面图上绘制选中断层的切割多边形"));
    m_btnDrawCut->setCheckable(true);
    bar->addWidget(m_btnAdd);
    bar->addWidget(m_btnRename);
    bar->addWidget(m_btnRemove);
    bar->addStretch(1);
    bar->addWidget(m_btnUndo);
    bar->addWidget(m_btnRedo);
    bar->addWidget(m_btnDrawCut);
    root->addLayout(bar);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels(QStringList{tr("断层"), tr("内容")});
    m_tree->setRootIsDecorated(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setStretchLastSection(true);
    root->addWidget(m_tree, 1);

    auto *detail = new QHBoxLayout();
    detail->setSpacing(4);
    auto *lblSide = new QLabel(tr("上盘方向："), this);
    m_cboHangingSide = new QComboBox(this);
    m_cboHangingSide->addItem(hangingSideLabel(FaultHangingSide::Unknown),
                              int(FaultHangingSide::Unknown));
    m_cboHangingSide->addItem(hangingSideLabel(FaultHangingSide::Left),
                              int(FaultHangingSide::Left));
    m_cboHangingSide->addItem(hangingSideLabel(FaultHangingSide::Right),
                              int(FaultHangingSide::Right));
    detail->addWidget(lblSide);
    detail->addWidget(m_cboHangingSide);
    detail->addStretch(1);
    root->addLayout(detail);

    m_lblStatus = new QLabel(QString(), this);
    m_lblStatus->setWordWrap(true);
    root->addWidget(m_lblStatus);

    connect(m_btnAdd, &QToolButton::clicked, this, &FaultManagerPanel::onAddFault);
    connect(m_btnRename, &QToolButton::clicked, this, &FaultManagerPanel::onRenameFault);
    connect(m_btnRemove, &QToolButton::clicked, this, &FaultManagerPanel::onRemoveFault);
    connect(m_btnUndo, &QToolButton::clicked, this, [this] {
        if (m_controller && m_controller->editStack())
            m_controller->editStack()->undo();
    });
    connect(m_btnRedo, &QToolButton::clicked, this, [this] {
        if (m_controller && m_controller->editStack())
            m_controller->editStack()->redo();
    });
    connect(m_btnDrawCut, &QToolButton::toggled, this, &FaultManagerPanel::onToggleCutDraw);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &FaultManagerPanel::onSelectionChanged);
    connect(m_tree, &QTreeWidget::itemChanged, this, &FaultManagerPanel::onItemChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (item && !item->data(0, Qt::UserRole).toString().isEmpty())
                    onRenameFault();
            });
    connect(m_cboHangingSide, &QComboBox::currentIndexChanged, this,
            &FaultManagerPanel::onHangingSideChanged);
}

void FaultManagerPanel::onAddFault()
{
    if (!m_controller)
        return;
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("新建断层"), tr("断层名："), QLineEdit::Normal,
                              QStringLiteral("F%1").arg(m_controller->faultSet().faultCount() + 1),
                              &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    m_controller->addFault(name.trimmed());
}

void FaultManagerPanel::onRenameFault()
{
    if (!m_controller)
        return;
    const QString faultId = selectedFaultId();
    const Fault *f = faultId.isEmpty() ? nullptr : m_controller->faultSet().faultById(faultId);
    if (!f)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("断层改名"),
                                               tr("新名称："), QLineEdit::Normal, f->name, &ok);
    if (!ok || name.trimmed().isEmpty() || name.trimmed() == f->name)
        return;
    if (!m_controller->renameFault(faultId, name.trimmed())) {
        m_lblStatus->setText(tr("改名失败：名称「%1」已被占用").arg(name.trimmed()));
        return;
    }
    m_lblStatus->clear();
}

void FaultManagerPanel::onRemoveFault()
{
    if (!m_controller)
        return;
    const QString faultId = selectedFaultId();
    if (faultId.isEmpty() || !m_controller->faultSet().faultById(faultId))
        return;
    m_controller->removeFault(faultId); // 可经撤销/重做回退
}

void FaultManagerPanel::onToggleCutDraw(bool checked)
{
    if (!checked) {
        teardownCutTool();
        emit cutDrawToggled(false);
        return;
    }
    if (!m_controller || !m_canvasCtl) {
        m_btnDrawCut->setChecked(false);
        return;
    }
    QgsMapCanvas *canvas = m_canvasCtl->canvas();
    if (!canvas) {
        m_btnDrawCut->setChecked(false); // 无画布：复位开关，不留假激活态
        return;
    }
    if (!m_cadDock) // CAD dock 随 canvas 生命周期（QgsMapToolAdvancedDigitizing 需要非空）
        m_cadDock = new QgsAdvancedDigitizingDockWidget(canvas, canvas);
    auto *tool = new PaleoDrawPolygonTool(canvas, m_cadDock);
    connect(tool, SIGNAL(constraintDrawn(QString)), this, SLOT(onCutDrawn(QString)));
    connect(tool, SIGNAL(drawAborted()), this, SLOT(onCutDrawAborted()));
    m_canvasCtl->setMapTool(tool);
    m_cutTool = tool;
    emit cutDrawToggled(true);
    m_lblStatus->setText(tr("绘制中：左键加点，右键收笔（≥3 点）；Esc 取消。目标层位 = %1")
                             .arg(m_controller->activeHorizon()));
}

void FaultManagerPanel::onCutDrawn(const QString &wkt)
{
    if (m_controller) {
        FaultHorizonCut cut;
        cut.wkt = wkt;
        // horizon 留空 → controller 取活动层位（SelectionContext 代理）
        m_controller->setCut(m_controller->activeFaultId(), cut);
    }
    // 保持工具激活以便连续绘制（解释习惯）；右键收笔后工具仍在画布上。
    if (m_lblStatus)
        m_lblStatus->setText(tr("切割多边形已记录（同断层同层位重画即替换）"));
}

void FaultManagerPanel::onCutDrawAborted()
{
    m_btnDrawCut->setChecked(false); // toggled → teardown
}

void FaultManagerPanel::onSelectionChanged()
{
    if (mSyncingSelection || !m_controller)
        return;
    const QString faultId = selectedFaultId();
    m_controller->setActiveFaultId(faultId); // 剖面拾取/画切割的目标断层
    // 面板选中 → 三视图联动（origin=fault_panel；联动高亮经 faultSelectionChanged 回声）
    m_controller->selectFaults(faultId.isEmpty() ? QStringList() : QStringList{faultId},
                               QStringLiteral("fault_panel"));
    // 选中变化时刷新上盘方向下拉
    const FaultHorizonCut *cut =
        faultId.isEmpty() ? nullptr
                          : m_controller->faultSet().cut(faultId, m_controller->activeHorizon());
    if (cut)
        m_cboHangingSide->setCurrentIndex(m_cboHangingSide->findData(int(cut->hangingSide)));
    else
        m_cboHangingSide->setCurrentIndex(0);
}

void FaultManagerPanel::onItemChanged(QTreeWidgetItem *item, int column)
{
    if (mRefreshing || !m_controller || column != 0)
        return;
    const QString faultId = item->data(0, Qt::UserRole).toString();
    if (faultId.isEmpty())
        return;
    m_controller->setFaultVisible(faultId, item->checkState(0) == Qt::Checked);
}

void FaultManagerPanel::onHangingSideChanged()
{
    if (mRefreshing || !m_controller)
        return;
    const QString faultId = selectedFaultId();
    if (faultId.isEmpty())
        return;
    const QString horizon = m_controller->activeHorizon();
    m_controller->setCutHangingSide(faultId, horizon,
                                    FaultHangingSide(m_cboHangingSide->currentData().toInt()));
}

void FaultManagerPanel::onFaultSetChanged()
{
    refreshTree();
    if (m_controller && !m_controller->lastError().isEmpty())
        m_lblStatus->setText(tr("落盘失败：%1").arg(m_controller->lastError()));
}

void FaultManagerPanel::onFaultSelectionChanged(const QStringList &faultIds)
{
    if (faultIds.isEmpty())
        return;
    if (faultIds.size() == 1 && faultIds.first() == selectedFaultId())
        return; // 自己发起的选择回声，无需搬树
    // 其他视图（地图/剖面）发起：镜像到树选中（屏蔽信号防再广播）
    mSyncingSelection = true;
    const QString &id = faultIds.first();
    if (QTreeWidgetItem *item = itemForFault(id)) {
        m_tree->clearSelection();
        item->setSelected(true);
        m_tree->setCurrentItem(item);
        m_controller->setActiveFaultId(id);
    }
    mSyncingSelection = false;
}

void FaultManagerPanel::refreshTree()
{
    if (!m_controller)
        return;
    mRefreshing = true;
    const QString keepSelected = selectedFaultId();
    m_tree->clear();
    for (const Fault &f : m_controller->faultSet().faults()) {
        auto *faultItem = new QTreeWidgetItem(m_tree);
        faultItem->setText(0, f.name);
        faultItem->setToolTip(0, tr("解释者：%1").arg(f.interpreter.isEmpty() ?
                                                          tr("（未记录）") : f.interpreter));
        faultItem->setCheckState(0, f.visible ? Qt::Checked : Qt::Unchecked);
        faultItem->setData(0, Qt::UserRole, f.id);
        faultItem->setText(1, tr("%1 棒 · %2 切割").arg(f.sticks.size()).arg(f.cuts.size()));
        for (const FaultStick &s : f.sticks) {
            auto *stickItem = new QTreeWidgetItem(faultItem);
            stickItem->setText(0, s.section.displayName);
            stickItem->setText(1, tr("TWT %1–%2 ms")
                                         .arg(qRound(s.twtMinMs()))
                                         .arg(qRound(s.twtMaxMs())));
        }
        for (const FaultHorizonCut &c : f.cuts) {
            auto *cutItem = new QTreeWidgetItem(faultItem);
            cutItem->setText(0, tr("切割 @%1").arg(c.horizon));
            cutItem->setText(1, hangingSideLabel(c.hangingSide));
        }
        faultItem->setExpanded(true);
    }
    mRefreshing = false;
    if (!keepSelected.isEmpty()) {
        if (QTreeWidgetItem *item = itemForFault(keepSelected))
            m_tree->setCurrentItem(item);
    }
}

void FaultManagerPanel::teardownCutTool()
{
    if (!m_cutTool)
        return;
    QgsMapTool *tool = m_cutTool;
    m_cutTool = nullptr;
    if (m_canvasCtl)
        m_canvasCtl->setMapTool(nullptr); // 正式 unset（deactivate）路径
    tool->deleteLater();
}

QTreeWidgetItem *FaultManagerPanel::itemForFault(const QString &faultId) const
{
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == faultId)
            return *it;
    }
    return nullptr;
}

} // namespace paleo::fault
