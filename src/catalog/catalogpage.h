// 层：数据
#pragma once
#include <QString>
#include <QVector>

// Values only. Keys are meaningful only in the caller's bound revision.
template<class T> struct CatalogPage {
  QVector<T> items;
  QVector<QString> itemKeys;
  QString lastKey;
  qint64 totalCount = 0;
  bool hasMore = false;
  int rowsVisited = 0;
};
