// 层：视图
#pragma once
#include <QWidget>

class PythonConsoleController;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

// ui/python/ — Python REPL 面板（方向68，**实验性**）。
// 交互式 python -i 会话的薄壳：回显会话输出（原文流，含提示符）、本地回显
// 输入行、回车发送。不做语法高亮/补全/对象内省（递延）。会话生命周期
//（启动/exit() 停止/递进强杀）在 services/PythonReplSession，经
// workflow/PythonConsoleController 转发；本面板只渲染 + 发意图。
class PythonReplPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit PythonReplPanel(PythonConsoleController *controller,
                             QWidget *parent = nullptr);

  public slots:
    void refresh(); // 按钮/输入可用性按会话态重画

  private:
    void appendText(const QString &text, bool stderrChannel);
    void appendSystem(const QString &text);
    void onSend();

    PythonConsoleController *m_controller = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QLineEdit *m_input = nullptr;
};
