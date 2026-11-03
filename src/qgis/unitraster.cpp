// 层：QGIS 封装
#include "unitraster.h"
#include "services/seismicunitinput.h"
#include <gdal.h>
#include <ogr_spatialref.h>
#include <cmath>
#include <memory>

namespace UnitRaster {
namespace {
using Dataset=std::unique_ptr<void,decltype(&GDALClose)>;
void transform(const paleo::strat::SeismicUnitInput &input,double *gt){
  gt[0]=input.extent.left();gt[1]=input.extent.width()/input.columns;gt[2]=0;
  gt[3]=input.extent.bottom();gt[4]=0;gt[5]=-input.extent.height()/input.rows;
}
bool failure(QString *error,const QString &message){if(error)*error=message;return false;}
}
bool writeInput(const paleo::strat::SeismicUnitInput &input,const QString &path,QString *error){
  if(input.rows<=0||input.columns<=0||input.samples.size()!=qint64(input.rows)*input.columns)
    return failure(error,QStringLiteral("层段输入尺寸无效"));
  GDALAllRegister();auto *driver=GDALGetDriverByName("GTiff");
  Dataset ds(driver?GDALCreate(driver,path.toUtf8().constData(),input.columns,input.rows,1,GDT_Float32,nullptr):nullptr,GDALClose);
  if(!ds)return failure(error,QStringLiteral("不能创建层段预测输入栅格"));
  double gt[6];transform(input,gt);
  if(GDALSetGeoTransform(ds.get(),gt)!=CE_None||GDALSetProjection(ds.get(),input.crs.toUtf8().constData())!=CE_None)
    return failure(error,QStringLiteral("不能设置层段输入的坐标参考"));
  auto values=input.samples;for(auto &v:values)if(!std::isfinite(v))v=-9999;
  const auto band=GDALGetRasterBand(ds.get(),1);GDALSetRasterNoDataValue(band,-9999);
  return GDALRasterIO(band,GF_Write,0,0,input.columns,input.rows,values.data(),input.columns,input.rows,GDT_Float32,0,0)==CE_None
      ||failure(error,QStringLiteral("不能写入层段预测输入栅格"));
}
bool maskOutput(const paleo::strat::SeismicUnitInput &input,const QString &path,QString *error){
  if(input.rows<=0||input.columns<=0||input.samples.size()!=qint64(input.rows)*input.columns)
    return failure(error,QStringLiteral("层段掩膜尺寸无效"));
  Dataset ds(GDALOpen(path.toUtf8().constData(),GA_Update),GDALClose);
  if(!ds||GDALGetRasterXSize(ds.get())!=input.columns||GDALGetRasterYSize(ds.get())!=input.rows||GDALGetRasterCount(ds.get())!=1)
    return failure(error,QStringLiteral("预测输出尺寸与层段网格不一致"));
  double expected[6],actual[6];transform(input,expected);
  if(GDALGetGeoTransform(ds.get(),actual)!=CE_None)return failure(error,QStringLiteral("预测输出缺少坐标定位"));
  for(int i=0;i<6;++i)if(std::abs(actual[i]-expected[i])>1e-7)return failure(error,QStringLiteral("预测输出坐标定位与层段网格不一致"));
  OGRSpatialReference source,target;source.SetFromUserInput(input.crs.toUtf8().constData());target.SetFromUserInput(GDALGetProjectionRef(ds.get()));
  if(!source.IsSame(&target))return failure(error,QStringLiteral("预测输出坐标系与层段网格不一致"));
  QVector<float> values(input.samples.size());const auto band=GDALGetRasterBand(ds.get(),1);
  if(GDALRasterIO(band,GF_Read,0,0,input.columns,input.rows,values.data(),input.columns,input.rows,GDT_Float32,0,0)!=CE_None)
    return failure(error,QStringLiteral("不能读取层段预测输出"));
  for(qsizetype i=0;i<values.size();++i)if(!std::isfinite(input.samples[i])||!std::isfinite(values[i]))values[i]=-9999;
  GDALSetRasterNoDataValue(band,-9999);
  return GDALRasterIO(band,GF_Write,0,0,input.columns,input.rows,values.data(),input.columns,input.rows,GDT_Float32,0,0)==CE_None
      ||failure(error,QStringLiteral("不能保存层段预测输出掩膜"));
}
bool maskOutputLikeInput(const QString &inputPath,const QString &outputPath,QString *error){
  Dataset ds(GDALOpen(inputPath.toUtf8().constData(),GA_ReadOnly),GDALClose);
  if(!ds||GDALGetRasterCount(ds.get())!=1)return failure(error,QStringLiteral("层段预测需要单波段输入"));
  paleo::strat::SeismicUnitInput input;input.columns=GDALGetRasterXSize(ds.get());input.rows=GDALGetRasterYSize(ds.get());
  double gt[6];if(GDALGetGeoTransform(ds.get(),gt)!=CE_None||gt[2]!=0||gt[4]!=0||gt[1]<=0||gt[5]>=0||qint64(input.rows)*input.columns>64000000)
    return failure(error,QStringLiteral("层段输入需要有效的北向上米制网格"));
  input.extent=QRectF(gt[0],gt[3]+gt[5]*input.rows,gt[1]*input.columns,-gt[5]*input.rows);
  input.crs=QString::fromUtf8(GDALGetProjectionRef(ds.get()));input.samples.resize(input.rows*input.columns);
  const auto band=GDALGetRasterBand(ds.get(),1);int hasNoData=0;const double nodata=GDALGetRasterNoDataValue(band,&hasNoData);
  if(GDALRasterIO(band,GF_Read,0,0,input.columns,input.rows,input.samples.data(),input.columns,input.rows,GDT_Float32,0,0)!=CE_None)
    return failure(error,QStringLiteral("不能读取层段输入掩膜"));
  for(auto &v:input.samples)if(!std::isfinite(v)||(hasNoData&&v==nodata))v=NAN;
  return maskOutput(input,outputPath,error);
}
}
