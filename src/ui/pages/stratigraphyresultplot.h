// 层：视图
#pragma once
#include <QWidget>
#include <paleo_data/interpretation.h>
class StratigraphyResultPlot:public QWidget {
public:
 using QWidget::QWidget;
 const paleo::data::InterpretationSeries* series=nullptr;
protected:
 void paintEvent(QPaintEvent*) override;
};
