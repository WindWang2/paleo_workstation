// 层：数据
#include "stratigraphyexport.h"
#include <QSaveFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>
bool exportStratigraphySeries(const paleo::data::InterpretationSeries& s,const QString& path,QString& error) {
 QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)){error=file.errorString();return false;}
 QTextStream out(&file);out<<"axis,value,quality\n";
 for(size_t i=0;i<std::min(s.axis.size(),s.values.size());++i)
  out<<QString::number(s.axis[i],'g',17)<<','<<(std::isfinite(s.values[i])?QString::number(s.values[i],'g',17):QString())<<','<<(i<s.quality.size()?int(s.quality[i]):0)<<'\n';
 out.flush();if(out.status()!=QTextStream::Ok||!file.commit()){error=file.errorString();return false;}return true;
}
