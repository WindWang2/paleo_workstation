// 层：视图
#pragma once

#include <QList>
#include <QString>
#include <QWidget>

#include "services/seismictaskservice.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QToolButton;
class QUndoStack;

namespace seismic {

class SeismicSectionDockWidget;

// D4.5/D4.6/D4.9 解释面板：拾取列表（定位/删除/重命名/导出）、undo/redo、
// 解释者名册、追踪参数（D4.2）、会话持久化（D4.8）、层位/断层资产登记
// （D4.3/D4.4）。会话模型在 dock（面板经指针读写）。
class SeismicPickPanel : public QWidget {
    Q_OBJECT

public:
    explicit SeismicPickPanel(SeismicSectionDockWidget *dock, QWidget *parent = nullptr);

    void refreshFromSession();
    void setUndoStack(QUndoStack *stack);
    QString currentInterpreter() const;
    QString currentHorizon() const;

signals:
    void sessionChanged();          // 模型变更（画布叠加/自动保存由 dock 响应）
    void locateRequested(int pickId);
    void trackRequested();          // 以选中拾取为种子追踪（D4.2）

private:
    void buildUi();
    void onAddInterpreter();
    void onExportCsv();
    void onRegisterHorizon();
    void onRegisterFault();
    void onSaveSession();
    void onLoadSession();
    void onDeleteSelected();
    void onRenameSelected();
    void onTrackClicked();

    SeismicSectionDockWidget *dock_ = nullptr;
    QTableWidget *table_ = nullptr;
    QLabel *emptyHint_ = nullptr; // goal/ui-experience-polish：空拾取指引
    QComboBox *cboInterpreter_ = nullptr;
    QLineEdit *editHorizon_ = nullptr;
    QSpinBox *spinTrackWindow_ = nullptr;
    QDoubleSpinBox *spinTrackThreshold_ = nullptr;
    QToolButton *btnUndo_ = nullptr;
    QToolButton *btnRedo_ = nullptr;
    QUndoStack *undoStack_ = nullptr;
};

} // namespace seismic
