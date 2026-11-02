// 层：数据
#include "wellpredictioninput.h"
#include "areawellchart.h"
#include "io/areamodel.h"
#include "io/welllogread.h"
#include "domain/welllogfacies.h"
#include "domain/wellnumeric.h"
#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <optional>
namespace paleo {
bool readModelWellPredictionInput(const QString &project,WellComposite::ComprehensiveWellData &data,
    QString *error,const std::function<bool()> &cancelled){
  paleo::data::DataService ds;std::string detail;WellComposite::ComprehensiveWellData native;
  if(!ds.openProjectReadOnly(area_model::directory(project).toStdString(),detail)){
    if(error)*error=QString::fromStdString(detail);return false;
  }
  if(!readAreaWellChart(ds,data.modelWellId,native,error,cancelled))return false;
  data.continuousCurves.clear();
  for(auto curve:native.continuousCurves){
    if(cancelled&&cancelled())return false;
    QVector<double> depths,values;for(float v:curve.depths)depths.append(v);for(float v:curve.values)values.append(v);
    QString reason;
    if(!normalizeDepthCurve(depths,values,QStringLiteral("m"),&curve,&reason)){if(error)*error=reason;return false;}
    data.continuousCurves.append(std::move(curve));
  }
  if(data.continuousCurves.isEmpty()){
    if(error)*error=native.dataWarnings.isEmpty()?QCoreApplication::translate("WellPredictionService","没有可读测井曲线"):native.dataWarnings.join('\n');return false;
  }
  for(const auto &formation:native.formationIntervals)
    if(std::none_of(data.formationIntervals.cbegin(),data.formationIntervals.cend(),[&](const auto &old){return old.name==formation.name;}))
      data.formationIntervals.append(formation);
  data.stratigraphyIntervals=native.stratigraphyIntervals;data.minDepth=native.minDepth;data.maxDepth=native.maxDepth;
  return true;
}
bool readFileWellPredictionCurves(const QVector<WellCurveRef> &refs,WellComposite::ComprehensiveWellData &well,
    QString *error,const std::function<bool()> &cancelled){
  QHash<QString,QPair<LasHeaderInfo,LasDoc>> docs;QHash<QString,WellLogFormat> formats;
  for(const auto &ref:refs){
    if(cancelled&&cancelled())return false;
    if(!docs.contains(ref.path)){
      const auto format=WellLogRead::detect(ref.path);formats.insert(ref.path,format);
      LasHeaderInfo header;LasDoc parsed;parsed.ok=WellLogRead::parse(ref.path,format,header,parsed.curves,&parsed.error);
      parsed.curveNames=header.curveNames;docs.insert(ref.path,{header,parsed});
    }
    const auto &header=docs[ref.path].first;const auto &doc=docs[ref.path].second;
    const bool explicitAxis=formats.value(ref.path)!=WellLogFormat::Las;
    const auto unitScale=doc.curves.isEmpty()?std::optional<double>():wellnumeric::depthScaleToMetres(doc.curves.first().unit);
    if(explicitAxis&&(!doc.ok||header.indexBasis!="MD"||!unitScale)){
      if(error)*error=QCoreApplication::translate("WellPredictionService","%1：预测需要已声明米或英尺单位的 MD 测深，当前基准为 %2；请检查索引与单位。%3")
          .arg(QFileInfo(ref.path).fileName(),header.indexBasis.isEmpty()?QCoreApplication::translate("WellPredictionService","未声明"):header.indexBasis,doc.error);
      well.continuousCurves.clear();return false;
    }
    if(header.indexBasis!="MD"||ref.column<1||ref.column>=doc.curves.size())continue;
    WellComposite::CurveData curve;curve.name=ref.mnemonic;curve.unit=doc.curves[ref.column].unit;QString reason;
    const auto &axis=doc.curves.first();
    if(normalizeDepthCurve(axis.values,doc.curves[ref.column].values,axis.unit,&curve,&reason))well.continuousCurves.append(std::move(curve));
  }
  return true;
}
}
