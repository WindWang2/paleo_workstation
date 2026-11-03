// 层：数据
#pragma once
#include <QVector>
#include "domain/grsandratio.h"

namespace paleo::singlefactor {
// MD 米；按相邻有效采样段的深度权重积分，线性求阈值交点。
// NULL/负 GR/缺深度/长缺口断开；不外推、不把缺口当泥岩或砂岩。
GrSandEstimate estimateGrSandRatio(const QVector<double> &md,const QVector<double> &gr,
    double top,double bottom,const GrSandOptions &options={});
}
