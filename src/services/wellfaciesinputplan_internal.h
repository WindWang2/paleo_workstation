// 层：数据
#pragma once
#include "wellfaciesinputplan.h"
#include <paleo_data/service.h>

namespace paleo::wellfacies::detail {
QString hash(const QByteArray &bytes);
QString hash(const QJsonObject &json);
QString occurrenceKey(const paleo::data::WellIntervalOccurrence &occurrence);
bool windows(paleo::data::DataService &service, const InputJob &job,
             QVector<OccurrenceWindow> &out, QString &code,QString *unitLabel=nullptr);
bool windows(const paleo::data::StratigraphicScheme &scheme,const InputJob &job,
             QVector<OccurrenceWindow> &out,QString &code);
bool curveRefs(paleo::data::DataService &service, const InputJob &job,
               QVector<CurveRef> &out, QString &code,const std::function<bool()> &cancelled={});
bool lithologyRef(paleo::data::DataService &service, const InputJob &job,
                 qlonglong &id, QJsonObject &manifest, QString &code,const std::function<bool()> &cancelled={});
}
