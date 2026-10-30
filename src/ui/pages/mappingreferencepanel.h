// 层：视图
#pragma once

#include <QHash>
#include <QList>
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
class ReferenceView;

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
  void setRemoveHint(bool lastView); // D-12：#547 允许关最后视图——仅换提示，不改可点性
  QgsMapCanvas *canvas() const;

signals:
  void referenceRequested(const QString &layerId);
  void removeViewRequested();
  void referenceChanged(const QString &title, const QString &versionId);

private:
  void refreshSources();
  void updateCaption();
  void updateLink();
  static QString category(const QVariantMap &row);
  QVariantMap selectedSource() const;

  PaleoDecorationManager *m_decor = nullptr;
  ReferenceView *m_view = nullptr;
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
  // 每个证据视图宽度可独立调整（拖动右缘手柄）；0 表示按「一屏约四个」的默认宽度。
  // persist=true 时按视图序号写入 QSettings，下次打开沿用。
  void setViewWidth(MappingReferencePanel *view, int width, bool persist = false);
  int viewWidth(MappingReferencePanel *view) const;
  int visibleViewCount() const; // 当前视口内（至少部分）可见的视图数
  static constexpr int kDefaultViewCount = 4;
  static constexpr int kMinViewWidth = 260;
  static constexpr int kMaxViewWidth = 4000;
  QSize sizeHint() const override { return {640, 340}; }

signals:
  void referenceRequested(MappingReferencePanel *view, const QString &layerId);

private:
  void removeView(MappingReferencePanel *view);
  void layoutViews();
  void updateViewSizes();
  int defaultViewWidth() const;
  void saveWidths();
  bool eventFilter(QObject *watched, QEvent *event) override;
  QgsMapCanvas *m_mainCanvas; // 非拥有；宿主与现有参考画布共享生命周期。
  QgsProject *m_project;
  QScrollArea *m_scroll;
  QHBoxLayout *m_row;
  QLabel *m_count;
  QLabel *m_emptyHint = nullptr; // D-12：全部视图关闭后的空态引导
  QList<MappingReferencePanel *> m_views;
  QList<QWidget *> m_handles;                    // 与 m_views 一一对应的右缘拖动手柄
  QHash<MappingReferencePanel *, int> m_widths;  // 用户指定宽度；缺省走默认宽度
  QList<int> m_savedWidths;                      // QSettings 中按序号保存的宽度
  QVariantList m_sources;
  QString m_horizon;
  bool m_active = false;
};
