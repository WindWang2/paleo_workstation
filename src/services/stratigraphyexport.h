// 层：数据
#pragma once
#include <paleo_data/interpretation.h>
#include <QString>
bool exportStratigraphySeries(const paleo::data::InterpretationSeries& series,const QString& path,QString& error);
