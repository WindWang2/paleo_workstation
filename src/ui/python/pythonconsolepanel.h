// 层：视图
#pragma once
#include "../../workflow/pythonconsolecontroller.h"
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QTextBrowser;

// ui/python/ — Python 脚本控制台面板（方向68）。
//
// 层站位：本面板只渲染 + 只发意图。运行/取消/协议解释/历史全在
// workflow/PythonConsoleController；结果导入经 importRequested 信号交给
// 组装根（视图不碰 io）。导入可用性预判用 domain/projectclassifier 词表
//（视图白名单内的合法 include）。
//
// DESIGN.md：字体/色/spacing 一律经 PaleoTheme 取；输出区用 monoFont，
// stdout/stderr/系统/错误四通道按 text/warning/textMuted/error token 分色。
class PythonConsolePanel : public QWidget
{
  Q_OBJECT
  public:
    explicit PythonConsolePanel(PythonConsoleController *controller,
                                QWidget *parent = nullptr);

  public slots:
    // 装配根回写导入反馈等系统消息（面板不自己弹对话框）。
    void appendSystemLine(const QString &message);
    // 按 controller 状态重画按钮可用性（解释器缺失 → 运行禁用 + 引导）。
    void refresh();

  signals:
    void importRequested(const QString &path); // 组装根接 DataImportService

  private:
    void appendOutput(const QString &text, int channel);
    void onRunClicked();
    void onStopClicked();
    void onBrowseScript();
    void onBrowseWorkdir();
    void onRunFinished(const PythonConsoleController::RunRecord &record);
    void onResultSelectionChanged();
    void onImportClicked();
    void onHistoryActivated(QListWidgetItem *item);
    void addResultItem(const QString &path, const QString &kind,
                       const QString &message);
    void updateInterpreterRow();
    // 词表预判：type 非 unknown 才算「导入面可识别」。typeOut 带出分类结果。
    bool importable(const QString &path, QString *typeOut = nullptr) const;

    PythonConsoleController *m_controller = nullptr;
    QLabel *m_interpreterLabel = nullptr;
    QPushButton *m_recheckButton = nullptr;
    QLineEdit *m_scriptEdit = nullptr;
    QLineEdit *m_argsEdit = nullptr;
    QLineEdit *m_workdirEdit = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QTextBrowser *m_output = nullptr;
    QListWidget *m_resultsList = nullptr;
    QPushButton *m_importButton = nullptr;
    QListWidget *m_historyList = nullptr;
};
