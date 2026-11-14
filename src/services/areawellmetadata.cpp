// 层：数据
#include "areawellmetadata.h"
#include <paleo_data/service.h>
#include <algorithm>
#include <iterator>

bool readBoundedAreaWellMetadata(const paleo::data::DataService &service,const std::string &kind,long long wellId,
    int maxEntries,std::vector<paleo::data::DataEntry> &entries,std::string &error,
    const std::function<bool()> &cancelled) {
  entries.clear();error.clear();
  if(maxEntries<1||maxEntries>4096){error="invalid_metadata_budget";return false;}
  const auto fail=[&](const std::string &reason){entries.clear();error=reason;return false;};
  long long after=0;
  for(;;) {
    if(cancelled&&cancelled())return fail("cancelled");
    const auto limit=std::min(512,maxEntries-int(entries.size())+1);
    std::vector<paleo::data::DataEntry> page;bool more=false;
    if(!service.listDataPage(kind,wellId,after,limit,page,more,error))
      return fail(error=="DataPageValueBudgetExceeded"?"input_metadata_budget_exceeded":error);
    if(cancelled&&cancelled())return fail("cancelled");
    if(entries.size()+page.size()>size_t(maxEntries))return fail("input_metadata_budget_exceeded");
    for(const auto &row:page){if(row.id<=after)return fail("invalid_metadata_page");after=row.id;}
    entries.insert(entries.end(),std::make_move_iterator(page.begin()),std::make_move_iterator(page.end()));
    if(!more)return true;
    if(page.empty()||entries.size()==size_t(maxEntries))return fail("input_metadata_budget_exceeded");
  }
}
