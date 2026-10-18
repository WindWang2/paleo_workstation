// 层：视图
#pragma once

#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QToolButton;
class QHBoxLayout;
class QScrollArea;
class QgsMapCanvas;
class QgsMapLayer;
class QgsProject;
class PaleoDecorationManager;
class MappingReferenceCanvas;

// 私有参考画布，不接管主图图层、编辑目标或地图工具；来源由宿主编排。
class MappingReferencePanel : public QWidget
{
  Q_OBJECT
public:
  MappingReferencePanel(QgsMapCanvas *mainCanvas, QgsProject *project,
                        QWidget *parent = nullptr);
  ~MappingReferencePanel() override;
  void setSources(const QVariantList &sources, const QString &activeHorizon);
  void selectSource(const QString &id);
  QString sourceId() const;
  bool hasReference() const;
  // 接管独立图层，关闭/换源前停止渲染；metadata 为已保存版本的 extra。
  void setReference(QgsMapLayer *layer, const QVariantMap &metadata);
  void clearReference(const QString &message = {});
  void setViewActive(bool active);
  void setCanRemove(bool canRemove);
  QgsMapCanvas *canvas() const;

signals:
  void referenceRequested(const QString &layerId);
  void removeViewRequested();
  void referenceChanged(const QString &title, const QString &versionId);

private:
  void refreshSources();
  void updateCaption();
  void updateLink();
  static QString category(const QVariantMap &source);
  QVariantMap selectedSource() const;

  PaleoDecorationManager *m_decor = nullptr;
  MappingReferenceCanvas *m_view = nullptr;
  QComboBox *m_sources = nullptr;
  QToolButton *m_remove = nullptr;
  QToolButton *m_linked = nullptr;
  QLabel *m_empty = nullptr;
  QVariantList m_rows;
  QVariantMap m_metadata;
  QString m_activeHorizon;
};

// 单一停靠面板的内容：内部视图可增减，每个视图独立选择证据。
class MappingEvidencePanel : public QWidget
{
  Q_OBJECT
public:
  MappingEvidencePanel(QgsMapCanvas *mainCanvas, QgsProject *project,
                       QWidget *parent = nullptr);
  void setSources(const QVariantList &sources, const QString &activeHorizon);
  void clearReferences(const QString &message = {});
  void setViewActive(bool active);
  MappingReferencePanel *addView();
  MappingReferencePanel *showSource(const QString &id);
  QList<MappingReferencePanel *> views() const { return m_views; }
  QSize sizeHint() const override { return {640, 340}; }

signals:
  void referenceRequested(MappingReferencePanel *view, const QString &layerId);

private:
  void removeView(MappingReferencePanel *view);
  void layoutViews();
  QgsMapCanvas *m_mainCanvas; // 非拥有；宿主与现有参考画布共享生命周期。
  QgsProject *m_project;
  QScrollArea *m_scroll;
  QHBoxLayout *m_row;
  QLabel *m_count;
  QList<MappingReferencePanel *> m_views;
  QVariantList m_sources;
  QString m_horizon;
  bool m_active = false;
};
