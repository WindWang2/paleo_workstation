// 层：数据
#pragma once
#include "outsourceworkbook.h"
#include "welltable.h"

namespace paleo::io {
inline constexpr char sectionTopsProfile[] = "paleo_section_tops_v1";
WorkbookReadResult readSectionTopsCsv(const QString &text, int rowLimit, bool *limited);
WellTable sectionTopsTable(const WorkbookReadResult &workbook);
}
