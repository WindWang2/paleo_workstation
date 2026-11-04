// 层：数据
#pragma once
#include "domain/deviationsurvey.h"
enum class ProjectDeviationRead { Unbound, Ready, Failed };
ProjectDeviationRead readProjectDeviation(const QString& project, qlonglong wellId, qlonglong sourceId,
    std::optional<paleo::WellDeviationSurvey>& survey, QString* error);
