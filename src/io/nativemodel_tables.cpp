// 层：数据
#include "nativemodel_internal.h"
#include "dataimport_internal.h"
#include <paleo_data/service.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

namespace paleo::native_model {
namespace {
QString q(const std::string &v) { return QString::fromUtf8(v); }
double value(const QVariantMap &row, const char *name) {
  const auto v=row.value(QLatin1String(name)); return v.isNull()?qQNaN():v.toDouble();
}
void add(paleo::data::SourceTable &table, const QVariantMap &row) {
  const auto bytes=QJsonDocument::fromVariant(row).toJson(QJsonDocument::Compact);
  table.cells.push_back({++table.rowCount,1,bytes.toStdString()});
}
Read columns(const QString &path, const QString &kind, QList<QVariantMap> &rows, QString *error, WellParseReport *report) {
  paleo::data::DataService service; QVariantMap binding;
  const auto state=open(path,kind,service,binding,error); if(state!=Read::Ready)return state;
  if(report) {
    const auto saved=binding.value("parseReport").toMap();
    report->acceptedRows=saved.value("acceptedRows").toInt(); report->rejectedRows=saved.value("rejectedRows").toInt();
    report->sentinelHits=saved.value("sentinelHits").toInt(); report->blankCells=saved.value("blankCells").toInt();
    report->invalidCells=saved.value("invalidCells").toInt(); report->issues=saved.value("issues").toStringList();
  }
  std::string detail; std::vector<paleo::data::DataEntry> tables;
  const auto fail=[&] { if(error)*error=q(detail); return Read::Failed; };
  if(!service.listData("tables",0,tables,detail))return fail();
  const auto name=QStringLiteral("workstation:%1:v1").arg(kind);
  for(const auto &entry:tables) {
    if(q(entry.name)!=name)continue;
    for(long long offset=0;offset<entry.count;offset+=1000) {
      paleo::data::DataTable page;
      if(!service.readTable("tables",entry.id,offset,1000,page,detail))return fail();
      for(const auto &row:page.rows) {
        if(row.size()!=2) { detail="invalid workstation extension columns"; return fail(); }
        const auto document=QJsonDocument::fromJson(QByteArray::fromStdString(row[1]));
        if(!document.isObject()) { detail="invalid workstation extension record"; return fail(); }
        rows.append(document.object().toVariantMap());
      }
    }
    return Read::Ready;
  }
  detail="native model is missing workstation extension columns"; return fail();
}
}
void preserveWellColumns(paleo::data::ParsedFile &model,const dataimport_detail::FamilyContext &ctx) {
  // Preserve the source order and fields absent from the shared semantic model
  // (KB, bottom coordinates, top XY/time) in its public source-table store.
  paleo::data::SourceTable table; table.columnCount=1;
  table.sheetName=QStringLiteral("workstation:%1:v1").arg(ctx.cls.type).toStdString();
  if(ctx.cls.type==QLatin1String("well_head")) for(const auto &r:ctx.parsedHeads)
    add(table,{{"name",r.name},{"x",r.x},{"y",r.y},{"kb",r.kb},{"td",r.td},
               {"bottomX",r.bottomX},{"bottomY",r.bottomY},{"hasBottomX",r.hasBottomX},
               {"hasBottomY",r.hasBottomY},{"wellType",r.wellType}});
  if(ctx.cls.type==QLatin1String("well_stratification")) for(const auto &r:ctx.parsedTops)
    add(table,{{"well",r.wellName},{"top",r.topName},{"md",r.md},{"x",r.x},{"y",r.y},{"z",r.z},
               {"tvd",r.tvd},{"time",r.timeMs},{"hasMd",r.hasMd},{"hasX",r.hasX},{"hasY",r.hasY},
               {"hasTvd",r.hasTvd},{"hasTime",r.hasTime}});
  if(table.rowCount)model.sourceTables.push_back(std::move(table));
}
Read heads(const QString &path,QVector<WellHeadRecord> &rows,QString *error,WellParseReport *report) {
  QList<QVariantMap> saved; const auto state=columns(path,"well_head",saved,error,report);
  if(state!=Read::Ready)return state;
  QVector<WellHeadRecord> next;
  for(const auto &v:saved) {
    WellHeadRecord r; r.name=v.value("name").toString(); r.x=value(v,"x"); r.y=value(v,"y");
    r.kb=value(v,"kb"); r.td=value(v,"td"); r.bottomX=value(v,"bottomX"); r.bottomY=value(v,"bottomY");
    r.hasBottomX=v.value("hasBottomX").toBool(); r.hasBottomY=v.value("hasBottomY").toBool();
    r.wellType=v.value("wellType").toString(); next.append(r);
  }
  rows=std::move(next); return Read::Ready;
}
Read tops(const QString &path,QVector<WellTopRecord> &rows,QString *error,WellParseReport *report) {
  QList<QVariantMap> saved; const auto state=columns(path,"well_stratification",saved,error,report);
  if(state!=Read::Ready)return state;
  QVector<WellTopRecord> next;
  for(const auto &v:saved) {
    WellTopRecord r; r.wellName=v.value("well").toString(); r.topName=v.value("top").toString();
    r.md=value(v,"md"); r.x=value(v,"x"); r.y=value(v,"y"); r.z=value(v,"z");
    r.tvd=value(v,"tvd"); r.timeMs=value(v,"time");
    r.hasMd=v.value("hasMd").toBool(); r.hasX=v.value("hasX").toBool(); r.hasY=v.value("hasY").toBool();
    r.hasTvd=v.value("hasTvd").toBool(); r.hasTime=v.value("hasTime").toBool(); next.append(r);
  }
  rows=std::move(next); return Read::Ready;
}
Read timeDepth(const QString &path,TimeDepthTable &table,QString *error) {
  paleo::data::DataService service; QVariantMap binding;
  const auto state=open(path,"time_depth",service,binding,error); if(state!=Read::Ready)return state;
  std::string detail; std::vector<paleo::data::DataEntry> entries;
  const auto fail=[&] { if(error)*error=q(detail); return Read::Failed; };
  if(!service.listData("time_depth",0,entries,detail))return fail();
  TimeDepthTable next; next.wellName=binding.value("wellName").toString();
  for(const auto &entry:entries) {
    paleo::data::TimeDepthTable relation;
    if(!service.readTimeDepth(entry.id,relation,detail))return fail();
    const auto count=qsizetype(relation.depthTimePairs.size());
    if(next.rows.isEmpty())next.rows.resize(count);
    if(next.rows.size()!=count) { detail="inconsistent native time-depth relations"; return fail(); }
    for(qsizetype i=0;i<count;++i) {
      auto &r=next.rows[i]; const auto &p=relation.depthTimePairs[size_t(i)]; r.timeMs=p.timeValue;
      if(relation.depthInfo.depthType=="MD") { r.md=p.depthValue; r.hasMd=std::isfinite(r.md); }
      else if(relation.depthInfo.depthType=="TVD") { r.tvd=p.depthValue; r.hasTvd=std::isfinite(r.tvd); }
      else if(relation.depthInfo.depthType=="TVDSS")r.tvdss=p.depthValue;
    }
  }
  if(entries.size()!=3) { detail="incomplete native time-depth relations"; return fail(); }
  table=std::move(next); return Read::Ready;
}
Read deviation(const QString &path,DeviationTable &table,QString *error) {
  paleo::data::DataService service; QVariantMap binding;
  const auto state=open(path,"well_deviation",service,binding,error); if(state!=Read::Ready)return state;
  std::string detail; std::vector<paleo::data::DataEntry> entries;
  if(!service.listData("trajectories",0,entries,detail)||entries.size()!=1) {
    if(error)*error=detail.empty()?QStringLiteral("native deviation survey is missing"):q(detail); return Read::Failed;
  }
  paleo::data::TrajectorySurvey survey;
  if(!service.readTrajectory(entries.front().id,survey,detail)) { if(error)*error=q(detail); return Read::Failed; }
  DeviationTable next; next.wellName=binding.value("wellName").toString();
  for(const auto &p:survey.surveyPoints)next.stations.append({p.measuredDepth,p.inclinationDegrees,p.azimuthDegrees});
  table=std::move(next); return Read::Ready;
}
}
