// 层：数据
#pragma once
#include <QString>

namespace paleo::seismic {
inline bool isRawClusterProduct(const QString &kind) {
    return kind == QLatin1String("seismic_clusters") || kind == QLatin1String("seismic_cluster_patches");
}
}
