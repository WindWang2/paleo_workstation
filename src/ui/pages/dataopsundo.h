// 层：视图
// ui/pages/dataopsundo — P1（data-commands 2026-11-08）起薄转发：命令基类/
// 宏/撤销栈的可执行核心已下沉功能层（workflow/dataopscommandservice.h），
// 本头保持旧 include 路径与全部公开类型可用（datalist/entitypanel/tests
// 兼容面）。栈语义演进（执行回执、storeIntact、会话守卫）见功能层头。
#pragma once

#include "../../workflow/dataopscommandservice.h"
