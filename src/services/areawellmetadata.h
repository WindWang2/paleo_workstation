// 层：数据
#pragma once
#include <functional>
#include <string>
#include <vector>
namespace paleo::data { class DataService; struct DataEntry; }
// Worker-side, bounded keyset inventory. Never materializes the unbounded legacy listData result.
bool readBoundedAreaWellMetadata(const paleo::data::DataService &service,const std::string &kind,long long wellId,
    int maxEntries,std::vector<paleo::data::DataEntry> &entries,std::string &error,
    const std::function<bool()> &cancelled={});
