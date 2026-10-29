// 层：数据
#include "seismicdataprojection.h"

namespace paleo {
seismic::SgySliceImage seismicSectionImage(const data::SeismicSectionResult& section) {
 seismic::SgySliceImage image;image.width=section.columns;image.height=section.samples;
 image.values.resize(section.amplitudes.size());
 for(int column=0;column<section.columns;++column)for(int sample=0;sample<section.samples;++sample)
  image.values[size_t(section.samples-1-sample)*section.columns+column]=section.amplitudes[size_t(column)*section.samples+sample];
 seismic::SgyVolume::Recolorize(image);return image;
}
bool readSeismicDataSlice(const QString& path,seismic::SgySliceType type,int index,
    seismic::SgySliceImage& image,QString& error,const data::CancelCheck& cancel) {
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
 data::SeismicTimeSliceResult slice;
 if(!source.time(info.firstTime+index*info.sampleInterval,options,slice,detail,cancel)){error=QString::fromUtf8(detail);return false;}
 image.width=int(slice.crosslineNumbers.size());image.height=int(slice.inlineNumbers.size());image.values.resize(slice.amplitudes.size());
 for(int row=0;row<image.height;++row)for(int col=0;col<image.width;++col)
  image.values[size_t(image.height-1-row)*image.width+col]=slice.amplitudes[size_t(row)*image.width+col];
 seismic::SgyVolume::Recolorize(image);return true;
}
}
