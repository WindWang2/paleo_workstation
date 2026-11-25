// 层：视图
#pragma once
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>
class MappingWorkbench;
class QCheckBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QLineEdit;
class QPushButton;
class QToolButton;
class MappingEditBasis : public QWidget {
  Q_OBJECT
public:
  MappingEditBasis(MappingWorkbench *workbench, QWidget *parent = nullptr);
  void setHorizon(const QString &horizon);
  void setReferences(const QVariantList &references);
  void setExpanded(bool expanded);
  bool isExpanded() const;
signals:
  void commandRequested(const QString &action, const QVariantMap &parameters);

private:
  void buildAutoSection(QWidget *parent, class QVBoxLayout *layout);
  void buildManualSection(QWidget *parent, class QVBoxLayout *layout);
  void applyDraftToForm(const QVariantMap &draft);
  void onAutoExplanationReady(const QString &horizon, const QVariantMap &explanation);
  void updateState();

  MappingWorkbench *m_workbench;
  QString m_horizon;
  QToolButton *m_toggle = nullptr;
  QWidget *m_content = nullptr;
  QLabel *m_autoStatus = nullptr;
  QCheckBox *m_autoToggle = nullptr;
  QPushButton *m_generateBtn = nullptr;
  QPushButton *m_cancelBtn = nullptr;
  QLabel *m_draftPreview = nullptr;
  QPushButton *m_adoptBtn = nullptr;
  QLabel *m_state = nullptr;
  QListWidget *m_sources = nullptr;
  QPlainTextEdit *m_text = nullptr;
  QLineEdit *m_reason = nullptr;
  QVariantMap m_latestDraft;
  bool m_formEdited = false;
};
