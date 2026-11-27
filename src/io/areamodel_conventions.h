// 层：数据
#pragma once
#include <QString>
namespace paleo::data { class DataService; }
namespace paleo::area_model {
bool inheritSurveyHorizonConventions(paleo::data::DataService &service,QString *error);
QString verifiedHorizonConventionsStamp(const QString &project);
bool inheritProjectHorizonConventions(const QString &project,QString *error,const QString &verifiedStamp = {});
}
