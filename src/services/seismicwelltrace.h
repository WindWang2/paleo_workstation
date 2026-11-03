// 层：数据
#pragma once
#include "domain/seismic/sectionwellprojector.h"
#include <QString>
#include <vector>
namespace paleo {
struct SeismicWellTrace {
 double firstTimeMs=0,sampleIntervalMs=0;
 std::vector<float> amplitudes;
 int finiteSamples=0,missingSamples=0;
 long long sourceColumns=0;
 QString coordinateSystem,timeReference;
};
// Adjacent observed samples only: no sorting, bridging or endpoint extension.
bool wellPathGridPositions(const std::vector<seismic::WellTrajSample>& path,double first,double step,int count,
                          std::vector<double>& x,std::vector<double>& y,QString *error);
}
