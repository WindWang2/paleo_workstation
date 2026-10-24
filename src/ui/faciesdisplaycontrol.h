// 层：视图
#pragma once
#include "../domain/facieshierarchy.h"
#include <QWidget>
class QComboBox;
class QLabel;
class FaciesDisplayControl : public QWidget {
  Q_OBJECT
public:
  explicit FaciesDisplayControl(QWidget *parent = nullptr);
  static QString scaleHint(const QString &level, const FaciesHierarchy::ScaleThresholds &thresholds);
  void setState(const QString &layerId, const QString &title, const QString &mode,
                const QString &level, const FaciesHierarchy::ScaleThresholds &thresholds,
                int pinnedCount = 0);
signals:
  void displayModeRequested(const QString &layerId, const QString &mode);
  void thresholdsRequested(double faciesToSubfacies, double subfaciesToMicrofacies);
private:
  void editThresholds();
  QString m_layerId;
  FaciesHierarchy::ScaleThresholds m_thresholds;
  QComboBox *m_mode;
  QLabel *m_state;
};
