// 层：数据
#pragma once
#include <QString>
#include <functional>
#include "domain/wellsection.h"
namespace paleo::data { class DataService; }
namespace WellComposite { struct ComprehensiveWellData; }
bool readAreaWellChart(paleo::data::DataService &service, qlonglong well,
                      WellComposite::ComprehensiveWellData &out, QString *error,
                      const std::function<bool()> &cancelled = {});
// Interval-only read: no log samples, photos or source-file reparsing.
bool readAreaWellLithology(const QString &project, qlonglong well,
                          QVector<wellsection::LithoSegment> &out, QString *error);
