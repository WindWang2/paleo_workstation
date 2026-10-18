// 层：视图
#pragma once
#include <QWidget>
class DataCatalog;
class ProjectStatisticsPage : public QWidget {
  Q_OBJECT
public:
  explicit ProjectStatisticsPage(DataCatalog *catalog, QWidget *parent = nullptr);
};
