// 层：数据
#include "wellfileparsers.h"
#include "nativemodel.h"
#include <QFile>
namespace {
bool bytes(const QString &path,QByteArray &out,QString *error) {
  QFile file(path);
  if(!file.open(QIODevice::ReadOnly)) { if(error)*error=file.errorString(); return false; }
  out=file.readAll();
  if(file.error()!=QFile::NoError) { if(error)*error=file.errorString(); return false; }
  return true;
}
}
bool readWellHeads(const QString &path,QVector<WellHeadRecord> &rows,QString *error,WellParseReport *report) {
  if(report)*report={}; if(error)error->clear(); rows.clear();
  const auto state=paleo::native_model::heads(path,rows,error,report);
  if(state!=paleo::native_model::Read::Unbound) {
    return state==paleo::native_model::Read::Ready;
  }
  QByteArray text; if(!bytes(path,text,error))return false;
  rows=parseWellHeadText(text,report); return true;
}
bool readWellTops(const QString &path,QVector<WellTopRecord> &rows,QString *error,WellParseReport *report) {
  if(report)*report={}; if(error)error->clear(); rows.clear();
  const auto state=paleo::native_model::tops(path,rows,error,report);
  if(state!=paleo::native_model::Read::Unbound) {
    return state==paleo::native_model::Read::Ready;
  }
  QByteArray text; if(!bytes(path,text,error))return false;
  rows=parseWellTopsText(text,report); return true;
}
bool readTimeDepth(const QString &path,TimeDepthTable &table,QString *error) {
  table={}; if(error)error->clear();
  const auto state=paleo::native_model::timeDepth(path,table,error);
  if(state!=paleo::native_model::Read::Unbound)return state==paleo::native_model::Read::Ready;
  QByteArray text; if(!bytes(path,text,error))return false;
  WellParseReport report;
  table=parseTimeDepthText(text,&report);
  // 空表必须带原因出口：Petrel 整表拒收 / 全空行等解析性失败不能静默成
  //「成功但零行」——调用方（tdTableFor→连井剖面）靠 error 如实上报。
  if(table.rows.isEmpty()&&error) {
    *error=report.issues.isEmpty()
        ?QStringLiteral("时深表没有可用数据行")
        :report.issues.constFirst();
  }
  return !table.rows.isEmpty();
}
