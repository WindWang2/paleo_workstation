// 层：数据
#include "wellfileparsers.h"
#include "nativemodel.h"
#include "lasparser.h"
#include <QFile>
namespace {
bool bytes(const QString &path,QByteArray &out,QString *error) {
  QFile file(path);
  if(!file.open(QIODevice::ReadOnly)) { if(error)*error=file.errorString(); return false; }
  // A-05：与井表导入同一 LasParser::fileSizeLimit。超限返回 false，禁止读成空表成功。
  if(file.size()>LasParser::fileSizeLimit()) {
    if(error)*error=QStringLiteral("file too large (%1 MB limit): %2")
        .arg(LasParser::fileSizeLimit()/(1024*1024)).arg(path);
    return false;
  }
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
bool readTimeDepth(const QString &path,TimeDepthTable &table,QString *error,QString *parseNote) {
  table={}; if(error)error->clear(); if(parseNote)parseNote->clear();
  const auto state=paleo::native_model::timeDepth(path,table,error);
  if(state!=paleo::native_model::Read::Unbound)return state==paleo::native_model::Read::Ready;
  QByteArray text; if(!bytes(path,text,error))return false;
  WellParseReport report;
  table=parseTimeDepthText(text,&report);
  // false 只留给 IO；「解析完成但零可用行」（Petrel 整表拒收 / 全哨兵）是
  // 解析结论不是读取失败——原因走 parseNote（消费方分流：预览面板显示
  //「无时深表」，tdTableFor 把 note 记 lastError 如实上报）。
  if(table.rows.isEmpty()&&parseNote) {
    *parseNote=report.issues.isEmpty()
        ?QStringLiteral("时深表没有可用数据行")
        :report.issues.constFirst();
  }
  return true;
}
