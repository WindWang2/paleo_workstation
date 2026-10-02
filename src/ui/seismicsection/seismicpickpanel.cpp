// 层：视图
#include "seismicpickpanel.h"

#include "../paleotheme.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

#include "seismicsectiondockwidget.h"

namespace seismic {

namespace {
// goal/ui-experience-polish：按钮 chrome 收敛 token（原 kBtnStyle 字面量），
// 活体注册随主题重算。
QToolButton *mkBtn(const QString &text, const QString &tooltip)
{
    auto *btn = new QToolButton();
    btn->setText(text);
    btn->setToolTip(tooltip);
    PaleoTheme::applyThemedStyleSheet(btn, [] { return PaleoTheme::toolButtonStyleSheet(); });
    return btn;
}
} // namespace

SeismicPickPanel::SeismicPickPanel(SeismicSectionDockWidget *dock, QWidget *parent)
    : QWidget(parent), dock_(dock)
{
    buildUi();
}

void SeismicPickPanel::setUndoStack(QUndoStack *stack)
{
    undoStack_ = stack;
    if (btnUndo_ && stack)
    {
        btnUndo_->setEnabled(stack->canUndo());
        btnRedo_->setEnabled(stack->canRedo());
    }
}

void SeismicPickPanel::buildUi()
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 4, 6, 4);
    lay->setSpacing(4);

    // 行 1：解释者（D4.9）+ 层位名 + undo/redo（D4.6）
    auto *row1 = new QHBoxLayout();
    row1->addWidget(new QLabel(tr("解释者:")));
    cboInterpreter_ = new QComboBox();
    cboInterpreter_->setEditable(true);
    cboInterpreter_->setFixedWidth(110);
    row1->addWidget(cboInterpreter_);
    row1->addWidget(new QLabel(tr("层位:")));
    editHorizon_ = new QLineEdit(tr("H1"));
    editHorizon_->setFixedWidth(70);
    row1->addWidget(editHorizon_);
    row1->addStretch();
    btnUndo_ = mkBtn(tr("↶ 撤销"), tr("撤销上一次拾取变更"));
    btnRedo_ = mkBtn(tr("↷ 重做"), tr("重做"));
    row1->addWidget(btnUndo_);
    row1->addWidget(btnRedo_);
    lay->addLayout(row1);

    // 行 2：追踪参数（D4.2）+ 追踪 QC 行（goal/horizon-autotrack：覆盖率/
    // 均值置信度/双侧停因——失败区如实留空的口径）
    auto *row2 = new QHBoxLayout();
    row2->addWidget(new QLabel(tr("追踪窗(样):")));
    spinTrackWindow_ = new QSpinBox();
    spinTrackWindow_->setRange(8, 128);
    spinTrackWindow_->setValue(24);
    spinTrackWindow_->setFixedWidth(56);
    row2->addWidget(spinTrackWindow_);
    row2->addWidget(new QLabel(tr("相关阈值:")));
    spinTrackThreshold_ = new QDoubleSpinBox();
    spinTrackThreshold_->setRange(0.1, 0.99);
    spinTrackThreshold_->setSingleStep(0.05);
    spinTrackThreshold_->setValue(0.6);
    spinTrackThreshold_->setFixedWidth(52);
    row2->addWidget(spinTrackThreshold_);
    btnTrack_ = mkBtn(tr("▶ 追踪同相轴"), tr("以选中拾取（或最后拾取）为种子，局部互相关沿同相轴双向追踪"));
    row2->addWidget(btnTrack_);
    lblTrackSummary_ = new QLabel();
    lblTrackSummary_->setObjectName(QStringLiteral("trackSummaryLabel"));
    PaleoTheme::applyThemedStyleSheet(lblTrackSummary_, [] {
        return PaleoTheme::mutedCaptionStyleSheet();
    });
    row2->addWidget(lblTrackSummary_, 1);
    row2->addStretch();
    lay->addLayout(row2);

    // 行 3：拾取表（D4.5）
    table_ = new QTableWidget();
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels({tr("ID"), tr("IL"), tr("XL"), tr("TWT(ms)"),
                                       tr("置信度"), tr("解释者"), tr("层位")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    // goal/ui-experience-polish：静默空表补空态指引（拾取为空时可见）。
    emptyHint_ = new QLabel(tr("还没有拾取——在剖面上按住 Ctrl+左键 拾取同相轴，或「载入会话」恢复上次解释"));
    emptyHint_->setObjectName(QStringLiteral("pickEmptyHint"));
    emptyHint_->setAlignment(Qt::AlignCenter);
    emptyHint_->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(emptyHint_, [] {
        return PaleoTheme::mutedCaptionStyleSheet();
    });
    lay->addWidget(emptyHint_, 0);
    lay->addWidget(table_, 1);
    // 构造即空会话——指引常显（refreshFromSession 只在会话变化时跑）。
    emptyHint_->setVisible(true);

    // 行 4：操作按钮
    auto *row4 = new QHBoxLayout();
    auto *btnLocate = mkBtn(tr("定位"), tr("画布跳到选中拾取（线号+TWT）"));
    auto *btnDelete = mkBtn(tr("删除"), tr("删除选中拾取"));
    auto *btnRename = mkBtn(tr("重命名"), tr("改选中拾取所属层位名"));
    auto *btnCsv = mkBtn(tr("导出 CSV"), tr("拾取集导出 CSV"));
    auto *btnHorizon = mkBtn(tr("生成层位资产"), tr("拾取网格化 → DERIVED 版本登记 catalog"));
    auto *btnFault = mkBtn(tr("登记断层"), tr("断层集 → 矢量派生资产登记"));
    auto *btnSave = mkBtn(tr("保存会话"), tr("解释会话写入 <sgy>.seispicks.json"));
    auto *btnLoad = mkBtn(tr("载入会话"), tr("从伴生文件恢复解释会话"));
    row4->addWidget(btnLocate);
    row4->addWidget(btnDelete);
    row4->addWidget(btnRename);
    row4->addWidget(btnCsv);
    row4->addWidget(btnHorizon);
    row4->addWidget(btnFault);
    row4->addWidget(btnSave);
    row4->addWidget(btnLoad);
    row4->addStretch();
    lay->addLayout(row4);

    connect(btnLocate, &QToolButton::clicked, this, [this]() {
        const auto sel = table_->selectionModel()->selectedRows();
        if (!sel.isEmpty())
            emit locateRequested(table_->item(sel.first().row(), 0)->text().toInt());
    });
    connect(btnDelete, &QToolButton::clicked, this, &SeismicPickPanel::onDeleteSelected);
    connect(btnRename, &QToolButton::clicked, this, &SeismicPickPanel::onRenameSelected);
    connect(btnCsv, &QToolButton::clicked, this, &SeismicPickPanel::onExportCsv);
    connect(btnHorizon, &QToolButton::clicked, this, &SeismicPickPanel::onRegisterHorizon);
    connect(btnFault, &QToolButton::clicked, this, &SeismicPickPanel::onRegisterFault);
    connect(btnSave, &QToolButton::clicked, this, &SeismicPickPanel::onSaveSession);
    connect(btnLoad, &QToolButton::clicked, this, &SeismicPickPanel::onLoadSession);
    connect(btnTrack_, &QToolButton::clicked, this, &SeismicPickPanel::onTrackClicked);
    connect(btnUndo_, &QToolButton::clicked, this, [this]() {
        if (undoStack_)
            undoStack_->undo();
    });
    connect(btnRedo_, &QToolButton::clicked, this, [this]() {
        if (undoStack_)
            undoStack_->redo();
    });
}

void SeismicPickPanel::refreshFromSession()
{
    if (!dock_)
        return;
    const SeismicInterpretationSession &session = dock_->interpretationSession();
    table_->setRowCount(0);
    if (emptyHint_)
        emptyHint_->setVisible(session.picks.isEmpty());
    for (const SeismicPick &p : session.picks)
    {
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(QString::number(p.id)));
        table_->setItem(row, 1, new QTableWidgetItem(QString::number(p.inlineNo)));
        table_->setItem(row, 2, new QTableWidgetItem(QString::number(p.xlineNo)));
        table_->setItem(row, 3, new QTableWidgetItem(QString::number(p.twtMs, 'f', 1)));
        auto *conf = new QTableWidgetItem(QString::number(p.confidence, 'f', 2));
        conf->setForeground(p.confidence >= 0.75 ? QBrush(QColor(0x43A047))
                            : (p.confidence >= 0.5 ? QBrush(QColor(0xF29900))
                                                   : QBrush(QColor(0xE53935))));
        table_->setItem(row, 4, conf);
        table_->setItem(row, 5, new QTableWidgetItem(p.interpreter));
        table_->setItem(row, 6, new QTableWidgetItem(p.horizonName));
    }
    // 解释者名册（D4.9）
    const QString current = cboInterpreter_->currentText();
    cboInterpreter_->blockSignals(true);
    cboInterpreter_->clear();
    cboInterpreter_->addItems(session.interpreters);
    if (current.isEmpty() && !session.interpreters.isEmpty())
        cboInterpreter_->setCurrentText(session.interpreters.first());
    else
        cboInterpreter_->setCurrentText(current);
    cboInterpreter_->blockSignals(false);

    if (undoStack_)
    {
        btnUndo_->setEnabled(undoStack_->canUndo());
        btnRedo_->setEnabled(undoStack_->canRedo());
    }
}

QString SeismicPickPanel::currentInterpreter() const
{
    return cboInterpreter_->currentText().trimmed();
}

QString SeismicPickPanel::currentHorizon() const
{
    return editHorizon_->text().trimmed();
}

void SeismicPickPanel::onDeleteSelected()
{
    const auto sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty() || !dock_)
        return;
    const int id = table_->item(sel.first().row(), 0)->text().toInt();
    dock_->removePick(id);
}

void SeismicPickPanel::onRenameSelected()
{
    const auto sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty() || !dock_)
        return;
    const int id = table_->item(sel.first().row(), 0)->text().toInt();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("重命名层位"),
                                               tr("新层位名："), QLineEdit::Normal,
                                               editHorizon_->text(), &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    dock_->renamePickHorizon(id, name.trimmed());
}

void SeismicPickPanel::onExportCsv()
{
    if (!dock_)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("导出拾取 CSV"), QStringLiteral("picks.csv"), tr("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!SeismicTaskService::exportPicksCsv(dock_->interpretationSession().picks, path, &err))
        QMessageBox::warning(this, tr("导出失败"), err);
    else
        QMessageBox::information(this, tr("已导出"), tr("拾取已保存到:\n%1").arg(path));
}

void SeismicPickPanel::onRegisterHorizon()
{
    if (!dock_)
        return;
    QString err;
    const QString path = dock_->registerCurrentHorizonAsset(&err);
    if (path.isEmpty())
        QMessageBox::warning(this, tr("层位资产登记失败"), err);
    else
        QMessageBox::information(this, tr("层位资产已登记"),
                                 tr("DERIVED 版本已登记 catalog:\n%1").arg(path));
}

void SeismicPickPanel::onRegisterFault()
{
    if (!dock_)
        return;
    QString err;
    const QString path = dock_->registerCurrentFaultAsset(&err);
    if (path.isEmpty() && !err.isEmpty())
        QMessageBox::warning(this, tr("断层资产登记失败"), err);
    else if (!path.isEmpty())
        QMessageBox::information(this, tr("断层资产已登记"),
                                 tr("DERIVED 版本已登记 catalog:\n%1").arg(path));
}

void SeismicPickPanel::onSaveSession()
{
    if (!dock_)
        return;
    QString err;
    if (!dock_->saveInterpretationSession(&err))
        QMessageBox::warning(this, tr("会话保存失败"), err);
    else
        QMessageBox::information(this, tr("会话已保存"),
                                 tr("解释会话已写入:\n%1").arg(dock_->sessionFilePath()));
}

void SeismicPickPanel::onLoadSession()
{
    if (!dock_)
        return;
    QString err;
    if (!dock_->loadInterpretationSession(&err) && !err.isEmpty())
        QMessageBox::warning(this, tr("会话载入失败"), err);
    refreshFromSession();
}

void SeismicPickPanel::onTrackClicked()
{
    // goal/horizon-autotrack：追踪在途 → 按钮即取消
    if (dock_ && dock_->trackingActive())
    {
        dock_->cancelTracking();
        return;
    }
    // 种子 = 选中拾取，否则最后一条
    const auto sel = table_->selectionModel()->selectedRows();
    int seedId = -1;
    if (!sel.isEmpty())
        seedId = table_->item(sel.first().row(), 0)->text().toInt();
    dock_->setTrackSeedPick(seedId);
    dock_->setTrackOptions({spinTrackWindow_->value(), 12,
                            spinTrackThreshold_->value()});
    emit trackRequested();
}

void SeismicPickPanel::setTrackingActive(bool active)
{
    if (btnTrack_)
    {
        btnTrack_->setText(active ? tr("■ 取消追踪") : tr("▶ 追踪同相轴"));
        btnTrack_->setToolTip(active ? tr("取消在途追踪任务")
                                     : tr("以选中拾取（或最后拾取）为种子，局部互相关沿同相轴双向追踪"));
    }
    if (active && lblTrackSummary_)
        lblTrackSummary_->setText(tr("追踪中…"));
}

void SeismicPickPanel::showTrackReport(const SeismicTrackReport &report)
{
    if (!lblTrackSummary_)
        return;
    const int percent = report.totalTraces > 0
                            ? report.coveredTraces * 100 / report.totalTraces
                            : 0;
    lblTrackSummary_->setText(
        tr("覆盖 %1/%2 道（%3%）· 均值置信 %4 · %5")
            .arg(report.coveredTraces)
            .arg(report.totalTraces)
            .arg(percent)
            .arg(QString::number(report.meanConfidence, 'f', 2))
            .arg(report.stopSummary.isEmpty() ? tr("—") : report.stopSummary));
}

void SeismicPickPanel::showTrackError(const QString &error)
{
    if (lblTrackSummary_)
        lblTrackSummary_->setText(error);
}

} // namespace seismic
