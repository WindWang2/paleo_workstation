// 层：数据
#include "projectdata.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "io/wellfileparsers.h"
#include <paleo_data/service.h>
#include <QFile>
#include <cmath>

QVector<TdSample> ProjectDataFacade::tdTableFor(const QString &wellId) const {
  QVector<TdSample> out;m_lastError.clear();
  const QString path=assetFilePathFor(wellId,QStringLiteral("time_depth"));
  if(path.isEmpty())return out;
  paleo::data::DataService service;long long source=0;
  const auto state=paleo::area_model::openSource(path,service,source,&m_lastError);
  if(state==paleo::area_model::Read::Failed)return out;
  if(state==paleo::area_model::Read::Unbound) {
    if(!QFile::exists(path))return out;
    TimeDepthTable table;QString parseNote;
    if(!readTimeDepth(path,table,&m_lastError,&parseNote))return out;
    // 解析完成但零可用行（Petrel 整表拒收/全哨兵）——note 记 lastError
    // 如实上报（消费端「时深不可用：<原因>」），不再静默空表。
    if(table.rows.isEmpty()) { m_lastError=parseNote; return out; }
    for(const auto& row:table.rows)out.append({row.timeMs,row.hasTvd?row.tvd:qQNaN(),row.hasMd?row.md:qQNaN(),{}});
    return out;
  }
  const auto entity=m_catalog->entityById(wellId);
  long long nativeWell=entity.extra.value("areaModelWellId").toLongLong();std::string error;
  if(!nativeWell) {
    std::vector<paleo::data::WellSummary> wells;
    if(!service.listWells(wells,error)){m_lastError=QString::fromUtf8(error);return {};}
    for(const auto& well:wells)if(QString::fromUtf8(well.name)==entity.name) {
      if(nativeWell){m_lastError=tr("时深井名对应多个模型井");return {};}
      nativeWell=well.id;
    }
  }
  std::vector<paleo::data::DataEntry> entries;
  if(!service.verifySource(source,error)||!service.listSourceData("time_depth",source,entries,error)){m_lastError=QString::fromUtf8(error);return {};}
  std::vector<paleo::data::TimeDepthTable> tables;
  for(const auto& entry:entries)if(nativeWell&&entry.wellId==nativeWell) {
    paleo::data::TimeDepthTable table;if(!service.readTimeDepth(entry.id,table,error)){m_lastError=QString::fromUtf8(error);return {};}
    if(table.depthInfo.depthType=="MD"||table.depthInfo.depthType=="TVD")tables.push_back(std::move(table));
  }
  const paleo::data::TimeDepthTable *selected=nullptr;
  for(const auto& type:{"MD","TVD"}) {
    for(const auto& table:tables)if(table.depthInfo.depthType==type) {
      if(selected){m_lastError=tr("同一来源有多份时深关系，请先明确采用的关系");return {};}
      selected=&table;
    }
    if(selected)break;
  }
  if(!selected){m_lastError=tr("数据服务中未找到本井的 MD / TVD 时深关系");return {};}
  const auto& table=*selected;
  if((table.timeUnit!="ms"&&table.timeUnit!="s")||(table.timeType!="TWT"&&table.timeType!="OWT")||table.timeReference.empty()||
      (table.depthInfo.depthUnit!="m"&&table.depthInfo.depthUnit!="ft")) {
    m_lastError=tr("时深关系的深度单位、时间单位、单/双程或时间基准尚未确认");return {};
  }
  const double timeScale=(table.timeUnit=="s"?1000.:1.)*(table.timeType=="OWT"?2.:1.);
  const double depthScale=table.depthInfo.depthUnit=="ft"?0.3048:1.;
  for(const auto& pair:table.depthTimePairs) {
    TdSample row;row.timeMs=pair.timeValue*timeScale;row.timeReference=QString::fromUtf8(table.timeReference);
    if(table.depthInfo.depthType=="MD")row.md=pair.depthValue*depthScale;else row.tvd=pair.depthValue*depthScale;
    out.append(row);
  }
  // Preserve a second declared depth axis only for exactly the same time samples/datum.
  int otherAxes=0;for(const auto& other:tables)if(other.depthInfo.depthType!=table.depthInfo.depthType)++otherAxes;
  if(otherAxes==1)for(const auto& other:tables) {
    if(&other==selected||other.depthInfo.depthType==table.depthInfo.depthType||other.depthTimePairs.size()!=size_t(out.size())||
       other.timeReference!=table.timeReference||(other.timeUnit!="ms"&&other.timeUnit!="s")||
       (other.timeType!="TWT"&&other.timeType!="OWT")||(other.depthInfo.depthUnit!="m"&&other.depthInfo.depthUnit!="ft"))continue;
    const double time=(other.timeUnit=="s"?1000.:1.)*(other.timeType=="OWT"?2.:1.);
    bool same=true;for(qsizetype i=0;i<out.size();++i)
      if(!std::isfinite(out[i].timeMs)||!std::isfinite(other.depthTimePairs[size_t(i)].timeValue)||
         std::abs(out[i].timeMs-other.depthTimePairs[size_t(i)].timeValue*time)>1e-8){same=false;break;}
    if(!same)continue;
    const double depth=other.depthInfo.depthUnit=="ft"?.3048:1.;
    for(qsizetype i=0;i<out.size();++i) {
      const double value=other.depthTimePairs[size_t(i)].depthValue*depth;
      if(other.depthInfo.depthType=="MD")out[i].md=value;else out[i].tvd=value;
    }
  }
  return out;
}

// 解析口径与 tdTableFor 同源（资产路径→openSource 定位来源），但确认是
// 写路径：对本工程的工区模型以读写方式另开连接（openSource 恒只读），
// 落到服务侧 confirmTimeDepthSemantics（整来源生效、原值留库、复核问题
// 联动消解）。只进数据服务，不动 catalog 台账。
bool ProjectDataFacade::confirmTimeDepthSemantics(const QString &wellId,
                                                  const QString &timeUnit,
                                                  const QString &timeType,
                                                  const QString &timeReference,
                                                  bool negateTimes, QString *error) {
  const auto fail=[this,error](const QString &why){
    m_lastError=why;if(error)*error=why;return false;
  };
  const QString path=assetFilePathFor(wellId,QStringLiteral("time_depth"));
  if(path.isEmpty())return fail(tr("井未挂接时深资产，无可确认的来源"));
  paleo::data::DataService probe;long long source=0;
  const auto state=paleo::area_model::openSource(path,probe,source,&m_lastError);
  if(state==paleo::area_model::Read::Failed)return fail(m_lastError);
  if(state==paleo::area_model::Read::Unbound)
    return fail(tr("时深资产未进工区数据服务（外链裸文件），请先经工区导入再确认口径"));
  const QString root=paleo::area_model::directory(projectDir());
  if(root.isEmpty()||!QFile::exists(root+QStringLiteral("/data.sqlite")))
    return fail(tr("本工程未建立工区数据模型，无从确认时深口径"));
  paleo::data::DataService service;std::string serviceError;
  if(!service.openProject(root.toUtf8().toStdString(),serviceError))
    return fail(QString::fromUtf8(serviceError));
  if(service.isReadOnly())
    return fail(tr("工区数据模型以只读方式打开，不能保存时深口径确认"));
  const auto entity=m_catalog->entityById(wellId);
  long long nativeWell=entity.extra.value("areaModelWellId").toLongLong();
  if(!nativeWell) {
    std::vector<paleo::data::WellSummary> wells;
    if(!service.listWells(wells,serviceError))return fail(QString::fromUtf8(serviceError));
    for(const auto& well:wells)if(QString::fromUtf8(well.name)==entity.name) {
      if(nativeWell)return fail(tr("时深井名对应多个模型井"));
      nativeWell=well.id;
    }
  }
  std::vector<paleo::data::DataEntry> entries;
  if(!service.verifySource(source,serviceError)||!service.listSourceData("time_depth",source,entries,serviceError))
    return fail(QString::fromUtf8(serviceError));
  long long entryId=0;
  for(const auto& entry:entries)if(nativeWell&&entry.wellId==nativeWell) {
    if(entryId)return fail(tr("同一来源有多份时深关系，请先明确采用的关系"));
    entryId=entry.id;
  }
  if(!entryId)return fail(tr("数据服务中未找到本井的时深关系"));
  if(!service.confirmTimeDepthSemantics(entryId,timeUnit.toStdString(),
                                        timeType.toStdString(),
                                        timeReference.toStdString(),negateTimes,
                                        serviceError))
    return fail(QString::fromUtf8(serviceError));
  return true;
}
