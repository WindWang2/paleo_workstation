// 层：数据
#pragma once
#include <QString>
#include <functional>
namespace paleo::data { class DataService; }
namespace WellComposite { struct ComprehensiveWellData; }
bool readAreaWellChart(paleo::data::DataService &service, qlonglong well,
                      WellComposite::ComprehensiveWellData &out, QString *error,
                      const std::function<bool()> &cancelled = {});
