// 层：数据
#pragma once
#include "seismicdataaccess.h"
#include "../domain/seismic/sgyvolume.h"

namespace paleo {
seismic::SgySliceImage seismicSectionImage(const data::SeismicSectionResult& section);
bool readSeismicDataSlice(const QString& path,seismic::SgySliceType type,int index,
    seismic::SgySliceImage& image,QString& error,const data::CancelCheck& cancel={});
}
