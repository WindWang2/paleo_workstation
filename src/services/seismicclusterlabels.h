// 层：数据
#pragma once
#include "seismichorizoncluster.h"

struct SeismicClusterWell {
  QString id, versionId;
  double x = 0, y = 0; // already transformed to the output grid CRS
  int code = 0;
  QString coordinateSource, name;
};

// One vote per well. Only a strict majority yields a facies code. Unresolved
// cells in the derived grid use NoData; the input cluster grid is immutable.
SeismicClusterResult labelSeismicClusters(
    SeismicClusterResult grid, const QVector<SeismicClusterWell> &wells,
    const std::function<bool()> &cancel = {});
