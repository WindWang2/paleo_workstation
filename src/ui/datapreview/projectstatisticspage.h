// 层：视图
#pragma once
#include <QPointer>
#include <QVector>
#include <QWidget>
class DataCatalog;
class QGridLayout;
class QVBoxLayout;
class QScrollArea;
class QgsRubberBand;
class QgsVectorLayer;
class PreviewMapCanvas;
class PaleoDecorationManager;
class ProjectStatisticsPage : public QWidget
{
  Q_OBJECT
public:
  explicit ProjectStatisticsPage(DataCatalog *catalog, QWidget *parent = nullptr);
signals:
  void surveyMapRequested();

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void showEvent(QShowEvent *event) override;

private:
  void refresh();
  void refreshSurveyMap();
  void buildSurveyMapCard();
  void arrangeCards();
  QPointer<DataCatalog> m_catalog;
  QScrollArea *m_scroll = nullptr;
  QVBoxLayout *m_content = nullptr;
  QGridLayout *m_metrics = nullptr;
  QGridLayout *m_horizons = nullptr;
  QPointer<PreviewMapCanvas> m_mapCanvas;
  QPointer<QgsVectorLayer> m_wellsLayer;
  QPointer<QgsRubberBand> m_boundaryBand;
  QPointer<PaleoDecorationManager> m_decorations;
  QVector<QWidget *> m_metricCards, m_horizonCards;
  int m_columns = 0;
};
