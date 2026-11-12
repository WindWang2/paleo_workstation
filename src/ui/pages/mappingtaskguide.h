// 层：视图
#pragma once
#include <QWidget>

class MappingWorkbench;
class QLabel;
class QPushButton;

// 智能预测 / 智能编图右侧栏的任务步骤条。
// 编图里「相面编辑」依赖已启用的本次解释依据：没启用时分割、覆盖绘制和合并无法完成。
class MappingTaskGuide : public QWidget {
  Q_OBJECT
public:
  MappingTaskGuide(const QString &mode, MappingWorkbench *workbench, QWidget *page,
                   QWidget *parent = nullptr);

public slots:
  void refresh();

private:
  void buildSteps();
  void refreshPredict();
  void refreshCompose();
  void apply(int stage, const QString &hint, const QString &nextText, const QString &anchor,
             bool nextEnabled);
  void markStep(const QString &id, bool done, bool current, bool blocked, const QString &tip);
  void focusAnchor(const QString &name);
  QString m_mode;
  MappingWorkbench *m_workbench = nullptr;
  QWidget *m_page = nullptr;
  QLabel *m_hint = nullptr;
  QPushButton *m_next = nullptr;
};
