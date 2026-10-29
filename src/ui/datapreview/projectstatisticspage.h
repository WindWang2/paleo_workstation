// 层：视图
#pragma once
#include <QPointer>
#include <QVector>
#include <QWidget>
#include "../../services/previewdoc.h" // AreaPlanarRanges（值类型门面）
class DataCatalog;
class QGridLayout;
class QVBoxLayout;
class QScrollArea;
class PaleoDecorationManager;
class PreviewDocService;
class ProjectStatisticsPage : public QWidget
{
  Q_OBJECT
public:
  explicit ProjectStatisticsPage(DataCatalog *catalog, PreviewDocService *doc,
                                 QWidget *parent = nullptr);
signals:
  void surveyMapRequested();

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void showEvent(QShowEvent *event) override;

private:
  void refresh();
  void refreshSurveyMap();
  void requestAreaRanges();
  void buildSurveyMapCard();
  void arrangeCards();
  QPointer<DataCatalog> m_catalog;
  QPointer<PreviewDocService> m_doc;
  QScrollArea *m_scroll = nullptr;
  QVBoxLayout *m_content = nullptr;
  QGridLayout *m_metrics = nullptr;
  QGridLayout *m_horizons = nullptr;
  QPointer<QWidget> m_mapCanvas; // 看板工区地图（src/qgis/dashboardsurveymap 建，PW-004）
  QPointer<PaleoDecorationManager> m_decorations;
  QVector<QWidget *> m_metricCards, m_horizonCards;
  // 工区三视图范围（数据服务探测；catalog 变更后置脏重取，服务侧按库
  // stamp 缓存——同 stamp 命中同步回，不产生额外探测）。
  AreaPlanarRanges m_areaRanges;
  bool m_rangesFetched = false;
  int m_columns = 0;
};
