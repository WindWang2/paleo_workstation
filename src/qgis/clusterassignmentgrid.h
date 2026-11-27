// 层：QGIS 封装
#pragma once
#include "../services/seismichorizoncluster.h"
class QgisProjectService;
class QgsCoordinateTransformContext;
#include <memory>

using ClusterTransformSnapshot = std::shared_ptr<const QgsCoordinateTransformContext>;
ClusterTransformSnapshot clusterTransformContext(QgisProjectService *project);
SeismicClusterResult readClusterAssignmentGrid(const QString &file, const std::function<bool()> &cancel);
