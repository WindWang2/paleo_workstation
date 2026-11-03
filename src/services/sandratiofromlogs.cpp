// 层：数据
#include "sandratiofromlogs.h"
#include "algorithms/singlefactor/curvesandratio.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "io/lasalias.h"
#include "services/welllogset.h"
#include "domain/wellnumeric.h"
#include <paleo_data/service.h>
#include <algorithm>

namespace paleo::singlefactor {
WellGrSandResult sandRatioFromLogs(const DataCatalog *catalog,const QString &project,
    const QString &wellId,qlonglong nativeId,double top,double bottom,
    const GrSandOptions &options,const std::function<bool()> &cancelled){
  WellGrSandResult out;QStringList problems;
  const auto consider=[&](const QVector<double> &md,const QVector<double> &gr,const QString &name,
                          const QVariantMap &identity,const QString &path,const QString &version){
    const auto estimate=estimateGrSandRatio(md,gr,top,bottom,options);
    if(!estimate.ok){problems.append(estimate.reason);return;}
    if(out.estimate.ok&&out.estimate.coveredMetres>=estimate.coveredMetres)return;
    out.estimate=estimate;out.provenance=identity;
    out.paths=path.isEmpty()?QStringList{}:QStringList{path};
    out.versionIds=version.isEmpty()?QStringList{}:QStringList{version};
    out.source=QStringLiteral("GR 曲线估算 · %1 · 覆盖 %2% · 阈值 %3").arg(name)
        .arg(estimate.coverage*100,0,'f',1).arg(estimate.thresholdGr,0,'g',7);
  };
  if(nativeId>0){
    paleo::data::DataService ds;std::string error;
    if(!ds.openProjectReadOnly(area_model::directory(project).toStdString(),error)){out.estimate.reason=QString::fromStdString(error);return out;}
    paleo::data::WellPlotOptions read;read.includeIntervals=false;read.includeMarkers=false;read.includePhotos=false;
    paleo::data::WellPlotData data;
    if(!ds.readWellPlotData(nativeId,read,data,error,cancelled)){out.estimate.reason=QString::fromStdString(error);return out;}
    for(const auto &group:data.logs){
      if(cancelled&&cancelled()){out.estimate.reason=QStringLiteral("提取已取消");return out;}
      const auto &logs=group.value;
      const auto scale=wellnumeric::depthScaleToMetres(QString::fromStdString(logs.depthInfo.depthUnit));
      if(logs.depthInfo.depthType!="MD"||!scale){problems.append(QStringLiteral("GR 需要已声明 MD 米/英尺深度"));continue;}
      QVector<double> md;for(double d:logs.samplingAxis.depthValues)md.append(wellnumeric::isUsable(d)?d*(*scale):qQNaN());
      for(const auto &curve:logs.logCurves){
        const auto name=QString::fromStdString(curve.curveName);
        if(LasAliasMap::shared().canonicalCurve(name)!="GR")continue;
        QVector<double> values;for(double v:curve.sampleValues)values.append(v);
        consider(md,values,name,{{"grLogSetId",group.entry.id},{"grSourceId",group.entry.sourceId},{"grCurve",name}}, {},{});
      }
    }
    // 原生组身份由 DataService 给出，目录版本补齐发布血缘（包括离线来源）。
    if(catalog)for(const auto &link:catalog->linksForEntity(wellId))if(link.role=="well_log"&&!link.unresolved){
      const auto version=catalog->currentVersion(link.assetId);out.versionIds.append(version.id);
      out.paths.append(DataCatalog::resolvedVersionPath(project,version));
    }
  }else{
    WellLogWarnings warnings;
    for(const auto &ref:WellLogSet::wellCurveIndex(catalog,project,wellId,&warnings)){
      if(cancelled&&cancelled()){out.estimate.reason=QStringLiteral("提取已取消");return out;}
      if(LasAliasMap::shared().canonicalCurve(ref.mnemonic.section('@',0,0).section('#',0,0))!="GR")continue;
      if(ref.indexBasis!="MD"){problems.append(QStringLiteral("GR 索引不是 MD 测深"));continue;}
      WellLogSet::WellCurveTvdSamples samples;QString error;
      if(!WellLogSet::readCurveTvd(ref,nullptr,&samples,&error)){problems.append(error);continue;}
      consider(samples.md,samples.values,ref.mnemonic,{{"grCurve",ref.mnemonic},{"grVersionId",ref.sourceVersionId},{"grColumn",ref.column}},ref.path,ref.sourceVersionId);
    }
    problems.append(warnings.messages);
  }
  if(out.estimate.ok){
    const auto &e=out.estimate;
    out.provenance.insert("valueMethod","gr_linear_vsh");out.provenance.insert("valueBasis","covered_md");
    out.provenance.insert("grSandBaseline",e.sandGr);out.provenance.insert("grShaleBaseline",e.shaleGr);
    out.provenance.insert("grShaleCutoff",options.shaleCutoff);out.provenance.insert("grThreshold",e.thresholdGr);
    out.provenance.insert("grMaxGapMetres",e.maxGapMetres);out.provenance.insert("coverage",e.coverage);
    out.provenance.insert("coveredThicknessMd",e.coveredMetres);out.provenance.insert("observedSandThicknessMd",e.sandMetres);
    out.provenance.insert("estimated",true);
  }else{problems.removeDuplicates();out.estimate.reason=problems.isEmpty()?QStringLiteral("缺少可计算的 GR 曲线"):problems.join(QStringLiteral("; "));}
  out.paths.removeAll({});out.paths.removeDuplicates();out.versionIds.removeAll({});out.versionIds.removeDuplicates();
  return out;
}
}
