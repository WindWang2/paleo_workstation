// 层：数据
#pragma once
#include "catalog/wellavailability.h"
#include <QVector>
#include <optional>

class DataCatalog;
namespace paleo::inventory {
struct Well {
  qlonglong id;
  QString name;
  double x, y;
  WellDataAvailability availability;
};
struct Volume {
  QString asset, name, fileName, timeUnit;
  int inlineMin = 0, inlineMax = 0, crosslineMin = 0, crosslineMax = 0;
  double interval = 0;
  bool hasLines = false;
};
struct Horizon {
  QString asset, name;
  qlonglong pointCount;
};
struct Area { QVector<Well> wells; QVector<Volume> volumes; QVector<Horizon> horizons; };
// Navigation needs object metadata only. No original file or sample arrays are opened.
std::optional<Area> readAreaInventory(const DataCatalog &catalog);
}
