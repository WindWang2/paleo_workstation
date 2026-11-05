// 层：数据
#pragma once
#include "domain/documentsource.h"
#include <functional>
namespace paleo::document {
// Worker-only snapshot inspection. Plain metadata reads no source body; quick
// verification reads <=64 KiB; paged discovery never opens the page directory.
SourceState inspectDocumentSource(const SourceRecord &source,bool verify,
                                 const std::function<bool()> &cancelled={});
}
