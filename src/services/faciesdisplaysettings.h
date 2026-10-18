// 层：数据
#pragma once
#include "../domain/facieshierarchy.h"
#include <QObject>

class FaciesDisplaySettings : public QObject {
  Q_OBJECT
public:
  static FaciesDisplaySettings &instance();
  FaciesHierarchy::ScaleThresholds thresholds() const { return m_thresholds; }
  bool save(const FaciesHierarchy::ScaleThresholds &thresholds, QString *error);
signals:
  void thresholdsChanged();
private:
  FaciesDisplaySettings();
  FaciesHierarchy::ScaleThresholds m_thresholds;
};
