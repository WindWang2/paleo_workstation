// 层：数据
#include "seismicdataprojection.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace paleo {
seismic::SgySliceImage seismicSectionImage(const data::SeismicSectionResult& section) {
 seismic::SgySliceImage image;image.width=section.columns;image.height=section.samples;
 image.values.resize(section.amplitudes.size());
 for(int column=0;column<section.columns;++column)for(int sample=0;sample<section.samples;++sample)
  image.values[size_t(section.samples-1-sample)*section.columns+column]=section.amplitudes[size_t(column)*section.samples+sample];
 seismic::SgyVolume::Recolorize(image);return image;
}
bool readSeismicDataSlice(const QString& path,seismic::SgySliceType type,int index,
    seismic::SgySliceImage& image,QString& error,const data::CancelCheck& cancel,const std::function<void(int,int)>& progress,bool bakeRgba,
    const NativeTimeTileCallback& tileReady,int previewAxis) {
 image={};SeismicDataAccess source;if(!source.open(path,&error))return false;std::string detail;
 if(type!=seismic::SgySliceType::Time) {
  data::SectionReadOptions options;options.fullResolution=true;options.maxValues=64000000;
  data::SeismicSectionResult section;
  if(!source.line(type==seismic::SgySliceType::Inline,index,options,section,detail,cancel)){error=QString::fromUtf8(detail);return false;}
  image=seismicSectionImage(section);return true;
 }
 data::SeismicFileInfo info;if(!source.inspect(info,detail,cancel)){error=QString::fromUtf8(detail);return false;}
 if(index<0||index>=info.sampleCount){error=QStringLiteral("时间采样号超出地震体范围");return false;}
 data::TimeSliceReadOptions options;options.fullResolution=true;options.maxValues=64000000;options.workerCount=2;
 if(previewAxis>0){options.fullResolution=false;options.maxInlineCount=previewAxis;options.maxCrosslineCount=previewAxis;}
 data::SeismicTimeSliceResult slice;
 if(!source.time(info.firstTime+index*info.sampleInterval,options,slice,detail,cancel,
    [&](const data::SeismicTimeSliceTile& tile){
     if(progress)progress(tile.completedTiles,tile.totalTiles);
     if(tileReady){
      auto out=std::make_shared<seismic::SgySliceImage>();out->width=tile.crosslineCount;out->height=tile.inlineCount;
      out->values.resize(tile.amplitudes.size());
      for(int row=0;row<out->height;++row)for(int col=0;col<out->width;++col)
       out->values[size_t(out->height-1-row)*out->width+col]=tile.amplitudes[size_t(row)*out->width+col];
      tileReady(std::move(out),tile.crosslineOffset,tile.totalInlineCount-tile.inlineOffset-tile.inlineCount,tile.completedTiles,tile.totalTiles);
     }
    })){
   error=QString::fromUtf8(detail);return false;
 }
 image.width=int(slice.crosslineNumbers.size());image.height=int(slice.inlineNumbers.size());image.values=std::move(slice.amplitudes);
 if(image.values.size()!=size_t(image.width)*image.height){image={};error=QStringLiteral("时间切片振幅尺寸不符");return false;}
 for(int row=0;row<image.height/2;++row){
  auto top=image.values.begin()+size_t(row)*image.width;
  auto bottom=image.values.begin()+size_t(image.height-1-row)*image.width;
  std::swap_ranges(top,top+image.width,bottom);
 }
 if(bakeRgba){seismic::SgyVolume::Recolorize(image);return true;}
 image.valueMin=std::numeric_limits<float>::max();image.valueMax=std::numeric_limits<float>::lowest();
 for(float value:image.values)if(std::isfinite(value)){image.valueMin=std::min(image.valueMin,value);image.valueMax=std::max(image.valueMax,value);}
 if(image.valueMin>image.valueMax){image.valueMin=0.f;image.valueMax=1.f;}
 return true;
}
}
