// 层：数据
#pragma once
#include "seismichorizoncluster.h"

struct SeismicClusterWell {
  QString id, versionId;
  double x = 0, y = 0; // already transformed to the output grid CRS
  int code = 0;
};

// One vote per well. Ties choose the lowest code; unobserved clusters use a
// reproducible random choice restricted to the observed well facies.
SeismicClusterResult labelSeismicClusters(
    SeismicClusterResult grid, const QVector<SeismicClusterWell> &wells,
    quint32 seed = 42, const std::function<bool()> &cancel = {});
