// 层：数据
#include "curvesandratio.h"
#include "domain/wellnumeric.h"
#include <algorithm>
#include <cmath>

namespace paleo::singlefactor {
namespace {
bool usableGr(double v){return v>=0&&!wellnumeric::isNullCurveValue(v);}
double quantile(QVector<double> values,double p){
  std::sort(values.begin(),values.end());
  const double index=p*(values.size()-1);const auto i=qsizetype(index);
  return values[i]+(values[std::min(i+1,values.size()-1)]-values[i])*(index-i);
}
}
GrSandEstimate estimateGrSandRatio(const QVector<double> &md,const QVector<double> &gr,
    double top,double bottom,const GrSandOptions &options){
  GrSandEstimate out;
  const auto fail=[&](const QString &reason){out.reason=reason;return out;};
  if(!std::isfinite(top)||!std::isfinite(bottom)||bottom<=top)
    return fail(QStringLiteral("缺少有效 MD 层段窗"));
  if(md.size()!=gr.size()||md.size()<2)return fail(QStringLiteral("GR 与 MD 样点不足或数量不一致"));
  if(!std::isfinite(options.shaleCutoff)||options.shaleCutoff<=0||options.shaleCutoff>=1||
     !std::isfinite(options.maxGapMetres)||options.maxGapMetres<0)
    return fail(QStringLiteral("GR 泥质阈值或缺口参数无效"));
  QVector<double> calibration,steps;int direction=0;double previous=qQNaN();
  for(qsizetype i=0;i<md.size();++i){
    if(wellnumeric::isUsable(md[i])&&usableGr(gr[i]))calibration.append(gr[i]);
    if(!wellnumeric::isUsable(md[i]))continue;
    const double step=md[i]-previous;previous=md[i];if(!std::isfinite(step))continue;
    if(step==0)return fail(QStringLiteral("GR 深度重复，不能唯一积分"));
    const int sign=step>0?1:-1;if(direction&&direction!=sign)return fail(QStringLiteral("GR 深度非单调"));
    direction=sign;if(i>0&&wellnumeric::isUsable(md[i-1]))steps.append(std::abs(step));
  }
  if(calibration.size()<2||steps.isEmpty())return fail(QStringLiteral("没有可积分的有效 GR 曲线段"));
  out.sandGr=std::isfinite(options.sandGr)?options.sandGr:quantile(calibration,.05);
  out.shaleGr=std::isfinite(options.shaleGr)?options.shaleGr:quantile(calibration,.95);
  if(out.sandGr<0||!(out.shaleGr>out.sandGr))return fail(QStringLiteral("GR 砂泥基线无有效差异，请设置砂/泥基线"));
  out.thresholdGr=out.sandGr+options.shaleCutoff*(out.shaleGr-out.sandGr);
  out.maxGapMetres=options.maxGapMetres>0?options.maxGapMetres:3*quantile(steps,.5);
  for(qsizetype i=1;i<md.size();++i){
    const auto ia=direction>0?i-1:md.size()-i,ib=direction>0?i:md.size()-i-1;
    const double a=md[ia],b=md[ib],ga=gr[ia],gb=gr[ib];
    if(!wellnumeric::isUsable(a)||!wellnumeric::isUsable(b)||!usableGr(ga)||!usableGr(gb)||b-a>out.maxGapMetres)continue;
    const double left=std::max(a,top),right=std::min(b,bottom);if(right<=left)continue;
    const auto value=[&](double d){return ga+(gb-ga)*(d-a)/(b-a);};
    const double gl=value(left),gright=value(right),length=right-left;
    out.coveredMetres+=length;++out.segments;
    if(gl<=out.thresholdGr&&gright<=out.thresholdGr)out.sandMetres+=length;
    else if((gl<out.thresholdGr)!=(gright<out.thresholdGr)){
      const double cross=left+(out.thresholdGr-gl)*length/(gright-gl);
      out.sandMetres+=gl<out.thresholdGr?cross-left:right-cross;
    }
  }
  if(!(out.coveredMetres>0))return fail(QStringLiteral("所选层段内无连续有效 GR 覆盖"));
  out.coverage=std::min(1.,out.coveredMetres/(bottom-top));
  out.ratio=std::clamp(out.sandMetres/out.coveredMetres,0.,1.);out.ok=true;return out;
}
}
