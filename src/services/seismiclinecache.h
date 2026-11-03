// 层：数据
#pragma once
#include <QString>
#include <paleo_data/service.h>
#include <functional>

namespace paleo {
using SeismicLineRead = std::function<bool(data::SeismicSectionResult&,std::string&)>;
// Worker-only, process-wide bounded raw-line cache; UI never waits on its locks.
bool cachedSeismicLine(const QString& path,const QString& context,bool isInline,int number,
    const data::SectionReadOptions& options,data::SeismicSectionResult& result,std::string& error,
    const data::CancelCheck& cancel,const SeismicLineRead& read);
void clearSeismicLineCache();
struct SeismicLineCacheStats {quint64 reads=0,hits=0,waits=0;size_t bytes=0,entries=0;};
SeismicLineCacheStats seismicLineCacheStats();
}
