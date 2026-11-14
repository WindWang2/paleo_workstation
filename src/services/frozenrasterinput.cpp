// 层：数据
#include "frozenrasterinput.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <gdal.h>
#include <cpl_string.h>
#include <cmath>
#include <limits>

namespace paleo {
bool normalizeRasterFeatureContract(const QJsonObject &input,QJsonObject &out) {
  out={};if(input.value("version")!=QJsonValue(1))return false;
  const QStringList keys{"version","kind","unit","domain","preprocessing"};
  for(auto it=input.begin();it!=input.end();++it)if(!keys.contains(it.key()))return false;
  out.insert("version",1);
  for(const auto &key:{"kind","unit","domain","preprocessing"}) {
    const auto value=input.value(QLatin1String(key));
    if(!value.isString() || value.toString().trimmed().isEmpty() || value.toString().toUtf8().size()>128)return false;
    out.insert(QLatin1String(key),value.toString().trimmed());
    const auto token=out.value(QLatin1String(key)).toString();
    if(token.contains(QChar(0)) || token.contains('\n') || token.contains('\r') ||
       QStringList{"unknown","unspecified","undefined","null","n/a","?"}.contains(token.toCaseFolded()) ||
       (token==QLatin1String("none") && QLatin1String(key)!=QLatin1String("domain")))return false;
  }
  const auto unit=out.value("unit").toString();
  if(unit==QLatin1String("1"))out.insert("unit","dimensionless");
  else if(unit==QLatin1String("metre") || unit==QLatin1String("meter"))out.insert("unit","m");
  else if(unit==QLatin1String("second"))out.insert("unit","s");
  else if(unit==QLatin1String("millisecond"))out.insert("unit","ms");
  return out.value("preprocessing")==QJsonValue("raw_float32_v1");
}
namespace {
constexpr qint64 maxSourceBytes=512LL*1024*1024;
bool fail(QString *error, const QString &code) { if (error) *error = code; return false; }
QString fingerprint(const QString &path, const std::function<bool()> &cancelled, QString *error,qint64 *totalBudget=nullptr) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { fail(error, "raster_source_unavailable"); return {}; }
  qint64 remaining=totalBudget?*totalBudget:maxSourceBytes;
  if(file.size()>remaining) {fail(error,"raster_source_byte_budget_exceeded");return {};}
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd()) {
    if (cancelled && cancelled()) { fail(error, "cancelled"); return {}; }
    auto bytes = file.read(1024 * 1024);
    if(bytes.size()>remaining) {fail(error,"raster_source_byte_budget_exceeded");return {};}
    remaining-=bytes.size();if(totalBudget)*totalBudget=remaining;
    if (bytes.isEmpty() && file.error() != QFile::NoError) { fail(error, "raster_read_failed"); return {}; }
    hash.addData(bytes);
  }
  return QString::fromLatin1(hash.result().toHex());
}
GDALDatasetH openRaster(const QString &path) {
  GDALAllRegister();const char *drivers[]={"GTiff",nullptr};
  return GDALOpenEx(path.toUtf8().constData(),GDAL_OF_RASTER|GDAL_OF_READONLY,drivers,nullptr,nullptr);
}
bool companionPaths(GDALDatasetH ds,const QString &main,QMap<QString,QString> &out,QString *error) {
  out.clear();const QFileInfo primary(main);qint64 bytes=primary.size();
  GDALGetMaskFlags(GDALGetRasterBand(ds,1));
  char **files=GDALGetFileList(ds);bool ok=true;
  for(int i=0;files && files[i];++i) {
    const QFileInfo file(QString::fromUtf8(files[i]));const auto path=file.canonicalFilePath();
    if(path==main)continue;
    if(path.isEmpty() || file.canonicalPath()!=primary.canonicalPath()) {ok=fail(error,"raster_dependency_unavailable");break;}
    const auto name=file.fileName();QString role;
    if(name.startsWith(primary.fileName()+'.'))role="suffix:"+name.mid(primary.fileName().size());
    else if(name.startsWith(primary.completeBaseName()+'.'))role="base:"+name.mid(primary.completeBaseName().size());
    if(role.isEmpty() || role.size()>256 || out.contains(role) || out.size()>=15) {ok=fail(error,"raster_dependency_unsupported");break;}
    if(file.size()<0 || file.size()>maxSourceBytes-bytes) {ok=fail(error,"raster_source_byte_budget_exceeded");break;}
    bytes+=file.size();out.insert(role,path);
  }
  CSLDestroy(files);return ok;
}
bool metadata(GDALDatasetH ds, FrozenRasterInput &out, QString *error) {
  if (!ds || GDALGetRasterCount(ds) != 1) return fail(error, "single_band_raster_required");
  const auto type=GDALGetRasterDataType(GDALGetRasterBand(ds,1));
  if(type==GDT_Unknown || GDALDataTypeIsComplex(type) || type==GDT_Int64 || type==GDT_UInt64)
    return fail(error,"raster_sample_type_unsupported");
  out.rows = GDALGetRasterYSize(ds); out.columns = GDALGetRasterXSize(ds);
  if (out.rows < 1 || out.columns < 1 || out.rows>32768 || out.columns>32768 || qint64(out.rows) * out.columns > 1024 * 1024)
    return fail(error, "tile_grid_budget_exceeded");
  double gt[6];
  if (GDALGetGeoTransform(ds, gt) != CE_None) return fail(error, "raster_grid_unknown");
  out.transform.clear();
  for (double value : gt) {
    if (!std::isfinite(value)) return fail(error, "raster_grid_invalid");
    out.transform.append(value);
  }
  if (gt[1] * gt[5] - gt[2] * gt[4] == 0) return fail(error, "raster_grid_invalid");
  out.projection = QString::fromUtf8(GDALGetProjectionRef(ds));
  if (out.projection.isEmpty()) return fail(error, "raster_crs_unknown");
  out.bandUnit = QString::fromUtf8(GDALGetRasterUnitType(GDALGetRasterBand(ds, 1)));
  const char *feature=GDALGetMetadataItem(GDALGetRasterBand(ds,1),"PALEO_FEATURE_CONTRACT",nullptr);
  if(!feature)feature=GDALGetMetadataItem(ds,"PALEO_FEATURE_CONTRACT",nullptr);
  out.featureContract={};
  if(feature) {
    if(qstrlen(feature)>8192)return fail(error,"source_feature_contract_budget_exceeded");
    QJsonParseError parse;const auto json=QJsonDocument::fromJson(QByteArray(feature),&parse);
    if(parse.error!=QJsonParseError::NoError || !json.isObject())return fail(error,"source_feature_contract_invalid");
    out.featureContract=json.object();
  }
  return true;
}
bool resolveFeature(FrozenRasterInput &input,const QJsonObject &catalog,QString *error) {
  QJsonObject source,declared;
  if(!input.featureContract.isEmpty() && !normalizeRasterFeatureContract(input.featureContract,source))
    return fail(error,"source_feature_contract_invalid");
  if(!catalog.isEmpty() && !normalizeRasterFeatureContract(catalog,declared))
    return fail(error,"source_feature_contract_invalid");
  if(source.isEmpty() && declared.isEmpty())return fail(error,"source_feature_contract_unavailable");
  if(!source.isEmpty() && !declared.isEmpty() && source!=declared)return fail(error,"source_feature_contract_mismatch");
  input.featureContract=source.isEmpty()?declared:source;input.catalogFeatureContract=declared;
  if(!input.bandUnit.isEmpty()) {
    auto band=input.featureContract;band.insert("unit",input.bandUnit);QJsonObject normalized;
    if(!normalizeRasterFeatureContract(band,normalized) || normalized.value("unit")!=input.featureContract.value("unit"))
      return fail(error,"source_feature_contract_mismatch");
  }
  return true;
}
}
QJsonObject FrozenRasterInput::toJson() const {
  QJsonArray gt; for (double value : transform) gt.append(value);
  QJsonObject companions;for(auto it=companionSha256.cbegin();it!=companionSha256.cend();++it)companions.insert(it.key(),it.value());
  return {{"path",path},{"versionId",versionId},{"sha256",sha256},{"rows",rows},
    {"columns",columns},{"transform",gt},{"projection",projection},{"bandUnit",bandUnit},
    {"featureContract",featureContract},{"catalogFeatureContract",catalogFeatureContract},{"companions",companions}};
}
QString FrozenRasterInput::bundleSha256() const {
  QJsonObject values{{"primary",sha256}};
  for(auto it=companionSha256.cbegin();it!=companionSha256.cend();++it)values.insert(it.key(),it.value());
  return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(values).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
bool FrozenRasterInput::fromJson(const QJsonObject &json, FrozenRasterInput &out, QString *error) {
  out = {}; out.path=json.value("path").toString(); out.versionId=json.value("versionId").toString();
  out.sha256=json.value("sha256").toString(); out.rows=json.value("rows").toInt();
  out.columns=json.value("columns").toInt(); out.projection=json.value("projection").toString();
  out.bandUnit=json.value("bandUnit").toString();
  if(!normalizeRasterFeatureContract(json.value("featureContract").toObject(),out.featureContract))
    return fail(error,"invalid_frozen_feature_contract");
  const auto catalog=json.value("catalogFeatureContract").toObject();
  if(!catalog.isEmpty() && !normalizeRasterFeatureContract(catalog,out.catalogFeatureContract))
    return fail(error,"invalid_frozen_feature_contract");
  const auto companions=json.value("companions").toObject();
  if(companions.size()>15)return fail(error,"invalid_frozen_dependencies");
  for(auto it=companions.begin();it!=companions.end();++it) {
    if(it.key().size()>256 || (!it.key().startsWith("suffix:.") && !it.key().startsWith("base:.")) ||
       !it.value().isString() || it.value().toString().size()!=64)return fail(error,"invalid_frozen_dependencies");
    out.companionSha256.insert(it.key(),it.value().toString());
  }
  for (const auto &value : json.value("transform").toArray()) {
    if (!value.isDouble() || !std::isfinite(value.toDouble())) return fail(error,"invalid_frozen_grid");
    out.transform.append(value.toDouble());
  }
  if (out.path.isEmpty() || out.versionId.isEmpty() || out.sha256.size()!=64 ||
      out.rows<1 || out.columns<1 || out.rows>32768 || out.columns>32768 || qint64(out.rows)*out.columns>1024*1024 ||
      out.transform.size()!=6 || out.projection.isEmpty()) return fail(error,"invalid_frozen_raster");
  return true;
}
bool freezeRasterInput(const QString &path, const QString &version, const QString &sha,
                       FrozenRasterInput &out, const std::function<bool()> &cancelled, QString *error,
                       const QJsonObject &catalog) {
  out = {};
  if (version.isEmpty() || sha.size()!=64) return fail(error,"raster_version_fingerprint_required");
  out.path=QFileInfo(path).canonicalFilePath(); out.versionId=version; out.sha256=sha;
  if(out.path.isEmpty())return fail(error,"raster_source_unavailable");
  const auto initial=fingerprint(out.path,cancelled,error);
  if(initial.isEmpty())return false;
  if(initial!=sha)return fail(error,"raster_source_changed");
  auto ds=openRaster(out.path);if(!ds)return fail(error,"raster_format_unavailable");
  QMap<QString,QString> companions;
  const bool ok=metadata(ds,out,error) && companionPaths(ds,out.path,companions,error);GDALClose(ds);
  if(!ok || !resolveFeature(out,catalog,error))return false;
  qint64 budget=maxSourceBytes-QFileInfo(out.path).size();
  for(auto it=companions.cbegin();it!=companions.cend();++it) {
    const auto hash=fingerprint(it.value(),cancelled,error,&budget);if(hash.isEmpty())return false;
    out.companionSha256.insert(it.key(),hash);
  }
  FrozenRasterReader check(out);return check.validate(cancelled,error);
}
struct FrozenRasterReader::Impl { FrozenRasterInput input; GDALDatasetH ds=nullptr; };
FrozenRasterReader::FrozenRasterReader(FrozenRasterInput input):d(std::make_unique<Impl>()) { d->input=std::move(input); }
FrozenRasterReader::~FrozenRasterReader() { if(d->ds)GDALClose(d->ds); }
bool FrozenRasterReader::validate(const std::function<bool()> &cancelled, QString *error) const {
  qint64 budget=maxSourceBytes;
  const auto actual=fingerprint(d->input.path,cancelled,error,&budget);
  if(actual.isEmpty())return false;
  if(actual!=d->input.sha256)return fail(error,"raster_source_changed");
  auto ds=openRaster(d->input.path);if(!ds)return fail(error,"raster_source_unavailable");
  QMap<QString,QString> paths;FrozenRasterInput current;
  const bool ok=metadata(ds,current,error) && companionPaths(ds,d->input.path,paths,error);GDALClose(ds);
  if(!ok || !resolveFeature(current,d->input.catalogFeatureContract,error))return false;
  if(current.rows!=d->input.rows || current.columns!=d->input.columns || current.transform!=d->input.transform ||
     current.projection!=d->input.projection || current.featureContract!=d->input.featureContract)
    return fail(error,"raster_grid_or_feature_changed");
  if(paths.keys()!=d->input.companionSha256.keys())return fail(error,"raster_dependencies_changed");
  for(auto it=paths.cbegin();it!=paths.cend();++it) {
    const auto hash=fingerprint(it.value(),cancelled,error,&budget);if(hash.isEmpty())return false;
    if(hash!=d->input.companionSha256.value(it.key()))return fail(error,"raster_dependencies_changed");
  }
  return true;
}
bool FrozenRasterReader::open(const std::function<bool()> &cancelled, QString *error) {
  if(d->ds || !validate(cancelled,error)) return false;
  d->ds=openRaster(d->input.path);
  FrozenRasterInput actual;
  if(!metadata(d->ds,actual,error)) return false;
  if(!resolveFeature(actual,d->input.catalogFeatureContract,error))return false;
  if(actual.rows!=d->input.rows || actual.columns!=d->input.columns ||
     actual.transform!=d->input.transform || actual.projection!=d->input.projection ||
     actual.bandUnit!=d->input.bandUnit || actual.featureContract!=d->input.featureContract)
    return fail(error,"raster_grid_or_feature_changed");
  return true;
}
bool FrozenRasterReader::read(int row,int col,int rows,int cols,QVector<float> &out,
                             const std::function<bool()> &cancelled,QString *error) {
  out.clear();
  if(cancelled && cancelled())return fail(error,"cancelled");
  if(!d->ds || row<0 || col<0 || rows<1 || cols<1 || qint64(row)+rows>d->input.rows ||
     qint64(col)+cols>d->input.columns || qint64(rows)*cols>2048*2048)
    return fail(error,"raster_window_invalid");
  QVector<double> raw(qsizetype(rows)*cols);
  auto band=GDALGetRasterBand(d->ds,1);
  if(GDALRasterIO(band,GF_Read,col,row,cols,rows,raw.data(),cols,rows,GDT_Float64,0,0)!=CE_None)
    return fail(error,"raster_read_failed");
  int present=0; const double nodata=GDALGetRasterNoDataValue(band,&present);
  QVector<quint8> mask;
  if(!(GDALGetMaskFlags(band)&GMF_ALL_VALID)) {
    mask.resize(raw.size());
    if(GDALRasterIO(GDALGetMaskBand(band),GF_Read,col,row,cols,rows,mask.data(),cols,rows,GDT_Byte,0,0)!=CE_None)
      return fail(error,"raster_mask_read_failed");
  }
  QVector<float> values(raw.size());
  for(qsizetype i=0;i<raw.size();++i) {
    if(!std::isfinite(raw[i]) || (present && raw[i]==nodata) || (!mask.isEmpty() && !mask[i]))
      values[i]=std::numeric_limits<float>::quiet_NaN();
    else {
      if(std::abs(raw[i])>std::numeric_limits<float>::max())return fail(error,"raster_value_out_of_float32_range");
      values[i]=float(raw[i]);
    }
  }
  if(cancelled && cancelled())return fail(error,"cancelled");
  out=std::move(values); return true;
}
} // namespace paleo
