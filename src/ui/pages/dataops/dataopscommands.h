// 层：视图
// ui/pages/dataops/dataopscommands — P1（data-commands 2026-11-08）起薄转发：
// 具体命令对象（挂接/解挂/转移/设主/标签/改型/软删/实体/重命名）与
// DataOpsContext 的可执行核心已下沉功能层（workflow/dataopscommandservice.h，
// 视图零服务写路径的编排归 DataOpsCommandService），本头保持旧 include
// 路径与全部公开类型可用（兼容声明）。
#pragma once

#include "../../../workflow/dataopscommandservice.h"
