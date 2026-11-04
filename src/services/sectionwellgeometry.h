// 层：数据
#pragma once
#include "projectdata.h"
#include "domain/seismic/sectionwellprojector.h"

void attachSectionWellGeometry(seismic::SectionWellInfo &well, const QVector<TdSample> &rows,
    const std::optional<paleo::WellDeviationSurvey> &survey,
    const seismic::TimeDepthModel *md, const seismic::TimeDepthModel *tvd, double shiftMs);
bool requiresSpatialWellSampling(const seismic::SectionWellInfo& well);
bool observedMdTimeIsMonotone(const QVector<TdSample>& rows);
