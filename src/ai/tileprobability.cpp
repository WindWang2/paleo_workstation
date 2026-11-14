// 层：功能
#include "tileinference.h"
#include <cmath>
#include <algorithm>

bool probabilityGrid(const QVector<float> &values,int classes,int rows,int cols,
                     const QVector<bool> &valid,TileClassGrid *out,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return false;};
  const qint64 pixels=qint64(rows)*cols;
  if(!out || classes<1 || classes>kMaxTileClasses || rows<1 || cols<1 || pixels>kMaxTileGridPixels ||
     values.size()!=pixels*classes || (!valid.isEmpty() && valid.size()!=pixels))
    return fail("model_probability_shape_invalid");
  for(qsizetype p=0;p<pixels;++p) {
    if(!valid.isEmpty() && !valid[p])continue;
    double sum=0;
    for(int c=0;c<classes;++c) {
      const double value=values[c*pixels+p];
      if(!std::isfinite(value) || value<0 || value>1)return fail("model_probability_out_of_contract");
      sum+=value;
    }
    if(classes>1 && std::abs(sum-1)>1e-4)return fail("model_probability_sum_invalid");
  }
  softmaxGrid(values,classes,rows,cols,valid,out,"probabilities");
  if(classes==1)for(qsizetype p=0;p<pixels;++p) {
    if(!valid.isEmpty() && !valid[p])continue;
    const float probability=values[p];out->argmax[p]=probability>=0.5f?1:0;
    out->probMax[p]=std::max(probability,1-probability);
  }
  return true;
}
