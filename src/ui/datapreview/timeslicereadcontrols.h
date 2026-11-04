// 层：视图
#pragma once
class QComboBox;
class QHBoxLayout;
class QSettings;
class QWidget;

namespace paleo::ui {
struct TimeSliceReadControls {
  QComboBox *resolution;
  QComboBox *workers;
};
TimeSliceReadControls makeTimeSliceReadControls(QWidget *parent,QHBoxLayout *layout,
                                               QSettings *settings=nullptr);
}
