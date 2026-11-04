// 层：数据
#include "areamodel_conventions.h"
#include <paleo_data/service.h>

namespace paleo::area_model {
bool inheritSurveyHorizonConventions(paleo::data::DataService &service,QString *error) {
  std::string detail;std::vector<paleo::data::DataEntry> volumes,horizons;
  const auto fail=[&]{if(error)*error=QString::fromUtf8(detail);return false;};
  if(!service.listData("volumes",0,volumes,detail))return fail();
  // An explicitly declared, single survey supplies the common convention.
  // Multiple surveys require an explicit per-horizon association.
  if(volumes.size()!=1)return true;
  paleo::data::SeismicVolume volume;paleo::data::ProjectCoordinates coordinates;
  if(!service.readVolume(volumes.front().id,volume,detail)||
     !service.readProjectCoordinates(coordinates,detail))return fail();
  const auto &g=volume.geometry;const auto &s=volume.sampling;
  if(g.coordinateSystem.empty()||g.coordinateSystem!=coordinates.coordinateSystem||
     g.coordinateUnit!="m"||coordinates.coordinateUnit!="m"||
     (s.timeUnit!="ms"&&s.timeUnit!="s")||s.timeType!="TWT")return true;
  if(!service.listData("horizons",0,horizons,detail))return fail();
  for(const auto &entry:horizons){
    paleo::data::SeismicHorizon h;if(!service.readHorizon(entry.id,0,1,h,detail))return fail();
    if(h.parentVolumeId&&h.parentVolumeId!=volume.volumeId)continue;
    if(!h.coordinateSystem.empty()&&h.coordinateSystem!=g.coordinateSystem)continue;
    if(!h.coordinateUnit.empty()&&h.coordinateUnit!=g.coordinateUnit)continue;
    bool changed=false;
    const auto fill=[&](std::string &target,const std::string &value){if(target.empty()&&!value.empty()){target=value;changed=true;}};
    fill(h.coordinateSystem,g.coordinateSystem);fill(h.coordinateUnit,g.coordinateUnit);
    fill(h.timeUnit,s.timeUnit);fill(h.timeType,s.timeType);fill(h.timeReference,s.timeReference);
    // Metadata only: saveHorizonInfo records a change, never rewrites points.
    if(changed&&!service.saveHorizonInfo(entry.id,h,detail))return fail();
  }
  return true;
}
}
