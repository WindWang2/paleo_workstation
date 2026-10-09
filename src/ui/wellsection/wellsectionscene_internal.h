// 层：视图
#pragma once
#include "wellsectionscene.h"

#include "ui/paleotheme.h"

#include <QColor>

// wellsectionscene_internal — 分 TU（方向 98 拆分）共享的内部件：非公开
// helper 集中于此，各 wellsectionscene_<域>.cpp include；对外契约仍在
// wellsectionscene.h。零改动搬运（原单 TU 匿名 ns 成员）。
namespace wellsectionui {

// 高亮一律取 UI token primary（不随图件主题；与层位 chip 同语义）。
inline QColor highlightColor() { return PaleoTheme::tokens().primary; }

} // namespace wellsectionui
