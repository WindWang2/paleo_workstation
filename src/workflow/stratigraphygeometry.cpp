// 层：功能
#include "stratigraphygeometry.h"
#include <QJsonDocument>
#include <cmath>
#include <stdexcept>
using namespace paleo::stratigraphy;
namespace paleo::stratigraphy_native {
GeometryOptions geometry(paleo::data::DataService& service,const std::string& view) {
 GeometryOptions g;paleo::data::CorrelationProjectConventions c;std::string error;
 if(!service.readCorrelationProjectConventions(c,error))throw std::runtime_error(error);
 g.resolveFromSources=true;g.confirmedDepthUnit=c.depthUnit;g.confirmedLogDepthType=c.depthType;g.confirmedDepthType=c.depthType;
 g.confirmedWellTimeUnit=c.timeUnit;g.confirmedHorizonTimeUnit=c.timeUnit;g.allowProvisionalAlignment=c.allowProvisionalAlignment;g.policyEvidence=c.evidence;
 g.allowBottomExtrapolation=true;g.maximumBottomExtrapolationM=0;g.allowTopExtrapolation=true;g.maximumLogExtrapolationM=100;
 const auto j=QJsonDocument::fromJson(QByteArray::fromStdString(view)).object();
 const auto text=[&](const char* key,std::string& value){if(j.contains(key))value=j[key].toString().toStdString();};
 const auto number=[&](const char* key,double& value){if(j[key].isDouble())value=j[key].toDouble();};
 const auto flag=[&](const char* key,bool& value){if(j[key].isBool())value=j[key].toBool();};
 text("coordinateSystem",g.confirmedCoordinateSystem);text("coordinateUnit",g.confirmedCoordinateUnit);text("timeReference",g.confirmedTimeReference);text("mdReference",g.confirmedMdReference);
 text("wellTimeUnit",g.confirmedWellTimeUnit);text("horizonTimeUnit",g.confirmedHorizonTimeUnit);text("wellTimeType",g.confirmedWellTimeType);text("horizonTimeType",g.confirmedHorizonTimeType);
 text("depthUnit",g.confirmedDepthUnit);text("depthType",g.confirmedDepthType);text("logDepthType",g.confirmedLogDepthType);text("verticalDepthReference",g.confirmedVerticalDepthReference);text("policyEvidence",g.policyEvidence);
 number("wellTimeSign",g.wellTimeSign);number("horizonTimeSign",g.horizonTimeSign);number("wellOffsetMs",g.wellTimeOffsetMs);number("horizonOffsetMs",g.horizonTimeOffsetMs);
 number("maximumSurfaceEdge",g.maximumSurfaceEdge);if(g.maximumSurfaceEdge==0)g.maximumSurfaceEdge=paleo::data::Missing;
 number("maximumTopExtrapolationM",g.maximumTopExtrapolationM);number("topExtrapolationFitWindowM",g.topExtrapolationFitWindowM);
 number("maximumBottomExtrapolationM",g.maximumBottomExtrapolationM);number("bottomExtrapolationFitWindowM",g.bottomExtrapolationFitWindowM);number("maximumLogExtrapolationM",g.maximumLogExtrapolationM);
 flag("resolveFromSources",g.resolveFromSources);flag("allowProvisionalAlignment",g.allowProvisionalAlignment);flag("allowIrregularSurface",g.allowIrregularSurface);
 flag("allowOneWay",g.allowOneWayConversion);flag("allowTopExtrapolation",g.allowTopExtrapolation);flag("allowBottomExtrapolation",g.allowBottomExtrapolation);return g;
}
QJsonObject geometryJson(const GeometryOptions& g) {
 QJsonObject j;
 const auto text=[&](const char* key,const std::string& value){j[key]=QString::fromStdString(value);};
 text("coordinateSystem",g.confirmedCoordinateSystem);text("coordinateUnit",g.confirmedCoordinateUnit);text("timeReference",g.confirmedTimeReference);text("mdReference",g.confirmedMdReference);
 text("wellTimeUnit",g.confirmedWellTimeUnit);text("horizonTimeUnit",g.confirmedHorizonTimeUnit);text("wellTimeType",g.confirmedWellTimeType);text("horizonTimeType",g.confirmedHorizonTimeType);
 text("depthUnit",g.confirmedDepthUnit);text("depthType",g.confirmedDepthType);text("logDepthType",g.confirmedLogDepthType);text("verticalDepthReference",g.confirmedVerticalDepthReference);text("policyEvidence",g.policyEvidence);
 j["wellTimeSign"]=g.wellTimeSign;j["horizonTimeSign"]=g.horizonTimeSign;j["wellOffsetMs"]=g.wellTimeOffsetMs;j["horizonOffsetMs"]=g.horizonTimeOffsetMs;
 j["maximumSurfaceEdge"]=std::isfinite(g.maximumSurfaceEdge)?g.maximumSurfaceEdge:0;
 j["maximumTopExtrapolationM"]=g.maximumTopExtrapolationM;j["topExtrapolationFitWindowM"]=g.topExtrapolationFitWindowM;j["maximumBottomExtrapolationM"]=g.maximumBottomExtrapolationM;
 j["bottomExtrapolationFitWindowM"]=g.bottomExtrapolationFitWindowM;j["maximumLogExtrapolationM"]=g.maximumLogExtrapolationM;
 j["resolveFromSources"]=g.resolveFromSources;j["allowProvisionalAlignment"]=g.allowProvisionalAlignment;j["allowIrregularSurface"]=g.allowIrregularSurface;
 j["allowOneWay"]=g.allowOneWayConversion;j["allowTopExtrapolation"]=g.allowTopExtrapolation;j["allowBottomExtrapolation"]=g.allowBottomExtrapolation;return j;
}
}
