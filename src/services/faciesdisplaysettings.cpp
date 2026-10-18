// 层：数据
#include "faciesdisplaysettings.h"
#include <QSettings>

namespace {
constexpr auto faciesKey = "mapping/faciesToSubfaciesScale";
constexpr auto microfaciesKey = "mapping/subfaciesToMicrofaciesScale";
}
FaciesDisplaySettings &FaciesDisplaySettings::instance() {
  static FaciesDisplaySettings settings;
  return settings;
}
FaciesDisplaySettings::FaciesDisplaySettings() {
  QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  FaciesHierarchy::ScaleThresholds stored{
      settings.value(faciesKey, m_thresholds.faciesToSubfacies).toDouble(),
      settings.value(microfaciesKey, m_thresholds.subfaciesToMicrofacies).toDouble()};
  if (FaciesHierarchy::validThresholds(stored)) m_thresholds = stored;
}
bool FaciesDisplaySettings::save(const FaciesHierarchy::ScaleThresholds &thresholds, QString *error) {
  if (!FaciesHierarchy::validThresholds(thresholds)) {
    if (error) *error = tr("相／亚相界限的比例尺分母必须大于亚相／微相界限，且两者均大于零");
    return false;
  }
  QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  settings.setValue(faciesKey, thresholds.faciesToSubfacies);
  settings.setValue(microfaciesKey, thresholds.subfaciesToMicrofacies);
  settings.sync();
  if (settings.status() != QSettings::NoError) {
    if (error) *error = tr("自动切换比例尺设置保存失败");
    return false;
  }
  m_thresholds = thresholds;
  emit thresholdsChanged();
  return true;
}
