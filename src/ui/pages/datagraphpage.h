// 层：视图
#pragma once
#include "../../services/datagraph.h"
#include <QPointer>
#include <QWidget>
class PreviewDocService;
class DataGraphWorkflow;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QGraphicsView;
class QGraphicsScene;
class QGraphicsItem;
class QLabel;
class QTreeWidget;
class QPlainTextEdit;
class QPushButton;
class DataGraphPage : public QWidget {
  Q_OBJECT
public:
  explicit DataGraphPage(QWidget *parent = nullptr);
  void setDocService(PreviewDocService *doc);
  void setPresentation(const paleo::datagraph::Presentation &presentation);
  void setSelection(const QString &entityId, const QString &assetId,
                    const QString &versionId = QString());
  void openRelations(const QString &assetId,
                     const QString &versionId = QString(),
                     const QString &entityId = QString());
  void refresh();
  void openLineage(const QString &assetId, const QString &versionId);
  void setFeatureEvidence(const QVariantMap &snapshot, bool open = false);
signals:
  void assetFocusRequested(const QString &assetId);
  void entityFocusRequested(const QString &entityId);
  void versionFocusRequested(const QString &assetId, const QString &versionId);
  void assetActivated(const QString &assetId);
  void versionActivated(const QString &versionId);
  void wellSelected(const QString &entityId);
  void workspaceRequested(const QString &versionId, const QString &action);
  void taskCenterRequested();
  void evidenceTargetRequested(const QVariantMap &target);
  void managementRequested(const QString &action, const QString &assetId,
                           const QString &versionId);

protected:
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;

private:
  void request();
  void render(const paleo::datagraph::Graph &graph);
  void selectNode(const paleo::datagraph::Node &node, bool navigate);
  void activateNode(const paleo::datagraph::Node &node);
  QString selectedPath() const;
  DataGraphWorkflow *m_workflow;
  QPointer<PreviewDocService> m_doc;
  QComboBox *m_mode, *m_category, *m_stage;
  QLineEdit *m_search;
  QSpinBox *m_depth;
  QCheckBox *m_history;
  QGraphicsView *m_view;
  QGraphicsScene *m_scene;
  QLabel *m_summary, *m_status;
  QPlainTextEdit *m_details;
  QTreeWidget *m_relations;
  paleo::datagraph::Graph m_graph;
  QPushButton *m_back;
  QVector<paleo::datagraph::Query> m_backHistory;
  paleo::datagraph::Query m_queryState;
  bool m_hasQuery = false, m_restoring = false;
  QPushButton *m_open, *m_locate, *m_folder, *m_trace, *m_impact, *m_cancel, *m_map,
      *m_reference, *m_wells, *m_related, *m_manage, *m_export;
  QHash<QString, QGraphicsItem *> m_items;
  QVector<QGraphicsItem *> m_edges;
  paleo::datagraph::Node m_selected;
  QString m_focus;
  QString m_projectPath;
  QString m_graphMessage;
  paleo::datagraph::Presentation m_presentation;
  bool m_busy = false, m_navigating = false;
  QVariantMap m_featureEvidence;
  QComboBox *m_evidenceTypeFilter = nullptr, *m_reviewFilter = nullptr;
  QSpinBox *m_evidencePage = nullptr;
  QVariantMap nodeTarget(const paleo::datagraph::Node &node) const;
};
