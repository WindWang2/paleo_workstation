// 层：视图
#pragma once
#include <QWidget>
#include <QMap>
#include <QHash>
#include <QPointer>
class QAction;
class PaleoEditingToolbar;

class MappingWorkbench;
class QLabel;
class QPushButton;

// 智能预测 / 智能编图右侧栏的任务步骤条。
// 工作副本决定编辑目标，手动解释可选。
class MappingTaskGuide : public QWidget {
  Q_OBJECT
public:
  MappingTaskGuide(const QString &mode, MappingWorkbench *workbench, QWidget *page,
                   QWidget *parent = nullptr);

  void watchEditor(PaleoEditingToolbar *editor);

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
  QString resolveDrafts();
  void chooseDraft();
  void resumeDraft(const QString &id);
  QPointer<PaleoEditingToolbar> m_editor;
  QMap<QString, QString> m_drafts;
  QHash<QString, QPointer<QAction>> m_draftTools;
  QString m_activeDraft;
  quint64 m_projectEpoch = 0;
  QString m_mode;
  MappingWorkbench *m_workbench = nullptr;
  QWidget *m_page = nullptr;
  QLabel *m_hint = nullptr;
  QPushButton *m_next = nullptr;
};
