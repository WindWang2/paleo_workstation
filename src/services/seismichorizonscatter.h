// 层：数据
#pragma once
#include "seismichorizoncluster.h"
#include <QMap>

class SegyReader;
namespace SeismicHorizonDetail {
struct Sample { int cell, xline; double timeMs, x = 0, y = 0; };
using Lines = QMap<int, QVector<Sample>>;
bool readScatter(const QString &path, const SegyReader &reader, Lines *lines,
                 SeismicClusterResult *result, const std::function<void(int)> &progress,
                 const std::function<bool()> &cancel);
}
