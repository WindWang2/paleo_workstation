// 层：数据
#pragma once
#include "outsourceworkbook.h"
namespace paleo::io {
WorkbookReadResult readWellDelimitedTable(const QString &path);
WorkbookReadResult previewWellDelimitedTable(const QString &path, bool *limited);
}
