// 层：数据
#pragma once
#include "seismicdataaccess.h"
#include "../domain/seismic/sgyvolume.h"

namespace paleo {
using NativeTimeTileCallback=std::function<void(std::shared_ptr<const seismic::SgySliceImage>,int,int,int,int)>;
seismic::SgySliceImage seismicSectionImage(const data::SeismicSectionResult& section);
bool readSeismicDataSlice(const QString& path,seismic::SgySliceType type,int index,
    seismic::SgySliceImage& image,QString& error,const data::CancelCheck& cancel={},
    const std::function<void(int,int)>& progress={},bool bakeRgba=true,
    const NativeTimeTileCallback& tileReady={});
}
